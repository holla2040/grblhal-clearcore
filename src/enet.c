/*
  enet.c - Ethernet + networking services glue for the Teknic ClearCore
  (SAME53 GMAC + lwIP 2.1 NO_SYS + grblHAL networking plugin).

  Adapted for grblhal-clearcore from the grblHAL iMXRT1062 driver's enet.c:

  Part of grblHAL
  Copyright (c) 2018-2025 Terje Io

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  grblHAL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with grblHAL. If not, see <http://www.gnu.org/licenses/>.

  Changes vs. iMXRT1062: platform layer replaced with our gmac.c +
  ethernetif.c (SAME53 GMAC at NVIC prio 3, RX pumped from foreground);
  telnet-only service scope (no http/websocket/ftp/mqtt/mdns/ssdp yet);
  trimmed settings to services/hostname/ip-mode/ip/gateway/netmask/
  telnet-port. This file remains GPLv3 per its origin.
*/

#include "driver.h"

#if ETHERNET_ENABLE

#include <string.h>
#include <stdio.h>

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/dhcp.h>
#include <lwip/timeouts.h>

#include "grbl/report.h"
#include "grbl/task.h"
#include "grbl/nvs_buffer.h"

#include "networking/networking.h"

#include "gmac.h"
#include "ethernetif.h"

#define LINK_CHECK_INTERVAL 250

static char IPAddress[IP4ADDR_STRLEN_MAX], if_name[NETIF_NAMESIZE] = "";
static network_services_t services = {0}, allowed_services;
static nvs_address_t nvs_address;
static network_settings_t ethernet, network;
static char netservices[NETWORK_SERVICES_LEN] = "";
static network_flags_t network_status = {};
static struct netif eth0_netif;
static bool dhcp_running = false;

static network_info_t *get_info (const char *interface)
{
    static network_info_t info = {};

    if(interface == if_name) {

        memcpy(&info.status, &network, sizeof(network_settings_t));

        strcpy(info.status.ip, IPAddress);

        if(info.status.ip_mode == IpMode_DHCP) {
            *info.status.gateway = '\0';
            *info.status.mask = '\0';
        }

        info.interface = (const char *)if_name;
        info.is_ethernet = true;
        info.link_up = network_status.link_up;
        info.dhcp = network.ip_mode == IpMode_DHCP;
        info.mbps = 100;
        info.status.services = services;

        struct netif *netif = netif_default;

        if(netif) {
            if(network_status.link_up) {
                ip4addr_ntoa_r(netif_ip_gw4(netif), info.status.gateway, IP4ADDR_STRLEN_MAX);
                ip4addr_ntoa_r(netif_ip_netmask4(netif), info.status.mask, IP4ADDR_STRLEN_MAX);
            }
            sprintf(info.mac, MAC_FORMAT_STRING, netif->hwaddr[0], netif->hwaddr[1],
                     netif->hwaddr[2], netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);
        }

        return &info;
    }

    return NULL;
}

static void status_event_out (void *data)
{
    networking.event(if_name, (network_status_t){ .value = (uint32_t)data });
}

static void status_event_publish (network_flags_t changed)
{
    task_add_immediate(status_event_out, (void *)((network_status_t){ .flags = network_status, .changed = changed }).value);
}

static void netif_status_callback (struct netif *netif)
{
    if(netif->ip_addr.addr != 0) {

        ip4addr_ntoa_r(netif_ip_addr4(netif), IPAddress, IP4ADDR_STRLEN_MAX);

#if TELNET_ENABLE
        if(network.services.telnet && !services.telnet)
            services.telnet = telnetd_init(network.telnet_port == 0 ? NETWORK_TELNET_PORT : network.telnet_port);
#endif

        if(!network_status.ip_aquired) {
            network_status.ip_aquired = On;
            status_event_publish((network_flags_t){ .ip_aquired = On });
        }
    }
}

static void link_status_callback (struct netif *netif)
{
    bool isLinkUp = netif_is_link_up(netif);

    if(isLinkUp != network_status.link_up) {

        network_flags_t changed = { .link_up = On };

        if((network_status.link_up = isLinkUp)) {
            if(network.ip_mode == IpMode_DHCP && !dhcp_running)
                dhcp_running = dhcp_start(netif) == ERR_OK;
        } else if(network.ip_mode == IpMode_DHCP && network_status.ip_aquired) {
            changed.ip_aquired = On;
            network_status.ip_aquired = Off;
        }

        status_event_publish(changed);

        if(network_status.link_up && network.ip_mode == IpMode_Static)
            netif_status_callback(netif);
    }
}

/* Foreground pump: lwIP timers + RX + service polling. Runs every 1 ms
   via the core task system — never from an ISR. */
static void grbl_enet_poll (void *data)
{
    (void)data;

    ethernetif_poll(&eth0_netif);
    sys_check_timeouts();

#if TELNET_ENABLE
    if(network_status.link_up && services.telnet)
        telnetd_poll();
#endif
}

/* Slow poll: PHY link status via MDIO */
static void link_check (void *data)
{
    (void)data;

    bool up = gmac_link_up();

    if(up != netif_is_link_up(&eth0_netif)) {
        if(up)
            netif_set_link_up(&eth0_netif);
        else
            netif_set_link_down(&eth0_netif);
    }

    task_add_delayed(link_check, NULL, LINK_CHECK_INTERVAL);
}

static void enet_start_now (void *data)
{
    (void)data;

    ip4_addr_t ip, mask, gw;

    memcpy(&network, &ethernet, sizeof(network_settings_t));

    if(network.telnet_port == 0)
        network.telnet_port = NETWORK_TELNET_PORT;

    lwip_init();

    if(network.ip_mode == IpMode_Static) {
        ip4addr_aton(network.ip, &ip);
        ip4addr_aton(network.mask, &mask);
        ip4addr_aton(network.gateway, &gw);
    } else
        ip.addr = mask.addr = gw.addr = IPADDR_ANY;

    netif_add(&eth0_netif, &ip, &mask, &gw, NULL, ethernetif_init, netif_input);
    netif_set_default(&eth0_netif);

    netif_set_status_callback(&eth0_netif, netif_status_callback);
    netif_set_link_callback(&eth0_netif, link_status_callback);

    netif_set_up(&eth0_netif);
    netif_index_to_name(1, if_name);
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&eth0_netif, network.hostname);
#endif

    network_status.interface_up = On;
    status_event_publish((network_flags_t){ .interface_up = On });

    task_add_systick(grbl_enet_poll, NULL);
    task_add_delayed(link_check, NULL, LINK_CHECK_INTERVAL);
}

/* --- Settings (trimmed from iMXRT enet.c) --- */

static void ethernet_settings_load (void);
static void ethernet_settings_restore (void);
static status_code_t ethernet_set_ip (setting_id_t setting, char *value);
static char *ethernet_get_ip (setting_id_t setting);
static status_code_t ethernet_set_services (setting_id_t setting, uint_fast16_t int_value);
static uint32_t ethernet_get_services (setting_id_t id);

static const setting_group_detail_t ethernet_groups [] = {
    { Group_Root, Group_Networking, "Networking" }
};

static const setting_detail_t ethernet_settings[] = {
    { Setting_NetworkServices, Group_Networking, "Network Services", NULL, Format_Bitfield, netservices, NULL, NULL, Setting_NonCoreFn, ethernet_set_services, ethernet_get_services, NULL, { .reboot_required = On } },
    { Setting_Hostname, Group_Networking, "Hostname", NULL, Format_String, "x(64)", NULL, "64", Setting_NonCore, ethernet.hostname, NULL, NULL, { .reboot_required = On } },
    { Setting_IpMode, Group_Networking, "IP Mode", NULL, Format_RadioButtons, "Static,DHCP", NULL, NULL, Setting_NonCore, &ethernet.ip_mode, NULL, NULL, { .reboot_required = On } },
    { Setting_IpAddress, Group_Networking, "IP Address", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, ethernet_set_ip, ethernet_get_ip, NULL, { .reboot_required = On } },
    { Setting_Gateway, Group_Networking, "Gateway", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, ethernet_set_ip, ethernet_get_ip, NULL, { .reboot_required = On } },
    { Setting_NetMask, Group_Networking, "Netmask", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, ethernet_set_ip, ethernet_get_ip, NULL, { .reboot_required = On } },
    { Setting_TelnetPort, Group_Networking, "Telnet port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCore, &ethernet.telnet_port, NULL, NULL, { .reboot_required = On } },
};

static const setting_descr_t ethernet_settings_descr[] = {
    { Setting_NetworkServices, "Network services to enable." },
    { Setting_Hostname, "Network hostname." },
    { Setting_IpMode, "IP Mode." },
    { Setting_IpAddress, "Static IP address." },
    { Setting_Gateway, "Static gateway address." },
    { Setting_NetMask, "Static netmask." },
    { Setting_TelnetPort, "(Raw) Telnet port number listening for incoming connections." },
};

static void ethernet_settings_save (void)
{
    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&ethernet, sizeof(network_settings_t), true);
}

static status_code_t ethernet_set_ip (setting_id_t setting, char *value)
{
    ip4_addr_t addr;

    if(ip4addr_aton(value, &addr) != 1)
        return Status_InvalidStatement;

    status_code_t status = Status_OK;

    switch(setting) {
        case Setting_IpAddress:
            strncpy(ethernet.ip, value, IP4ADDR_STRLEN_MAX);
            break;
        case Setting_Gateway:
            strncpy(ethernet.gateway, value, IP4ADDR_STRLEN_MAX);
            break;
        case Setting_NetMask:
            strncpy(ethernet.mask, value, IP4ADDR_STRLEN_MAX);
            break;
        default:
            status = Status_Unhandled;
            break;
    }

    return status;
}

static char *ethernet_get_ip (setting_id_t setting)
{
    static char ip[IP4ADDR_STRLEN_MAX];

    switch(setting) {
        case Setting_IpAddress:
            strncpy(ip, ethernet.ip, IP4ADDR_STRLEN_MAX);
            break;
        case Setting_Gateway:
            strncpy(ip, ethernet.gateway, IP4ADDR_STRLEN_MAX);
            break;
        case Setting_NetMask:
            strncpy(ip, ethernet.mask, IP4ADDR_STRLEN_MAX);
            break;
        default:
            *ip = '\0';
            break;
    }

    return ip;
}

static status_code_t ethernet_set_services (setting_id_t setting, uint_fast16_t int_value)
{
    (void)setting;

    ethernet.services.mask = int_value & allowed_services.mask;

    return Status_OK;
}

static uint32_t ethernet_get_services (setting_id_t id)
{
    (void)id;

    return (uint32_t)ethernet.services.mask & allowed_services.mask;
}

static void ethernet_settings_restore (void)
{
    memset(&ethernet, 0, sizeof(network_settings_t));

    strcpy(ethernet.hostname, NETWORK_HOSTNAME);
    strcpy(ethernet.ip, NETWORK_IP);
    strcpy(ethernet.gateway, NETWORK_GATEWAY);
    strcpy(ethernet.mask, NETWORK_MASK);

    ethernet.ip_mode = (ip_mode_t)NETWORK_IPMODE;
    ethernet.telnet_port = NETWORK_TELNET_PORT;
    ethernet.services.mask = allowed_services.mask;

    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&ethernet, sizeof(network_settings_t), true);
}

static void ethernet_settings_load (void)
{
    if(!hal.nvs.memcpy_from_nvs((uint8_t *)&ethernet, nvs_address, sizeof(network_settings_t), true))
        ethernet_settings_restore();

    ethernet.services.mask &= allowed_services.mask;
}

static setting_details_t setting_details = {
    .groups = ethernet_groups,
    .n_groups = sizeof(ethernet_groups) / sizeof(setting_group_detail_t),
    .settings = ethernet_settings,
    .n_settings = sizeof(ethernet_settings) / sizeof(setting_detail_t),
    .descriptions = ethernet_settings_descr,
    .n_descriptions = sizeof(ethernet_settings_descr) / sizeof(setting_descr_t),
    .save = ethernet_settings_save,
    .load = ethernet_settings_load,
    .restore = ethernet_settings_restore
};

bool grbl_enet_start (void)
{
    if((nvs_address = nvs_alloc(sizeof(network_settings_t)))) {

        networking_init();

        hal.driver_cap.ethernet = On;
        networking.get_info = get_info;
        allowed_services.mask = networking_get_services_list((char *)netservices).mask;

        settings_register(&setting_details);

        /* GMAC + PHY up-front so the MAC is known; netif comes up via task */
        uint8_t mac[6];
        gmac_init(mac);
        ethernetif_set_mac(mac);

        task_run_on_startup(enet_start_now, NULL);
    }

    return nvs_address != 0;
}

#endif /* ETHERNET_ENABLE */
