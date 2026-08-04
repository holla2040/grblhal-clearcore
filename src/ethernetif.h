/*
 * ethernetif.h — lwIP netif driver entry points for grblhal-clearcore.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include "lwip/err.h"
#include "lwip/netif.h"

/*
 * Hand over the MAC address a previous gmac_init() programmed. Call before
 * netif_add() when the caller brought the GMAC up itself; ethernetif_init()
 * then skips hardware initialization. Without it, ethernetif_init() calls
 * gmac_init() on its own.
 */
void ethernetif_set_mac(const uint8_t mac[6]);

/*
 * netif_add() initializer. Names the interface "e0", installs the transmit
 * path and (unless ethernetif_set_mac() was called) brings up the GMAC:
 *
 *   netif_add(&netif, &ip, &mask, &gw, NULL, ethernetif_init, netif_input);
 *
 * netif_input dispatches to ethernet_input for us because the interface
 * carries NETIF_FLAG_ETHARP; passing ethernet_input directly is equivalent.
 *
 * NETIF_FLAG_LINK_UP is left clear — drive it from gmac_link_up() on a slow
 * timer with netif_set_link_up() / netif_set_link_down().
 */
err_t ethernetif_init(struct netif *netif);

/*
 * Pump received frames into lwIP. Call from the grblHAL foreground next to
 * sys_check_timeouts(); never from an ISR. Handles at most 8 frames per call
 * so one pass through the realtime loop stays bounded.
 */
void ethernetif_poll(struct netif *netif);

#endif /* ETHERNETIF_H */
