/*
 * usb_serial.h — TinyUSB CDC-ACM as a grblHAL stream.
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef __USB_SERIAL_H__
#define __USB_SERIAL_H__

#include "grbl/stream.h"

const io_stream_t *usb_serialInit (void);   /* also inits USB hw + TinyUSB */
void usb_poll (void);                       /* pump: call regularly (foreground) */

#endif
