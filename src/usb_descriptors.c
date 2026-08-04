/*
 * usb_descriptors.c — TinyUSB device/config/string descriptors for the
 * grblHAL ClearCore CDC console. VID/PID = Teknic's REAL application-port
 * IDs (0x2890:0x8022 — their flash_clearcore.cmd matches this to find the
 * app port for the 1200-baud touch; bootloader port is 0x2890:0x0022).
 * The 0x239A:0x80CD pair in the community board JSON was wrong.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "tusb.h"

#define USB_VID 0x2890
#define USB_PID 0x8022

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1
};

uint8_t const *tud_descriptor_device_cb (void)
{
    return (uint8_t const *)&desc_device;
}

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb (uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: English (0x0409) */
    "Teknic",                       /* 1: manufacturer */
    "ClearCore grblHAL",            /* 2: product */
    "grblhal-cc",                   /* 3: serial */
    "grblHAL CDC",                  /* 4: CDC interface */
};

uint16_t const *tud_descriptor_string_cb (uint8_t index, uint16_t langid)
{
    static uint16_t _desc_str[32];
    uint8_t chr_count;

    (void)langid;

    if(index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if(index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;

        const char *str = string_desc_arr[index];

        chr_count = strlen(str);
        if(chr_count > 31)
            chr_count = 31;

        for(uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
