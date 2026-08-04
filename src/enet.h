/*
 * enet.h — Ethernet + networking services for grblhal-clearcore.
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef __ENET_H__
#define __ENET_H__

#include <stdbool.h>

bool grbl_enet_init (void);     /* driver_init: NVS area + settings registration */
bool grbl_enet_start (void);    /* driver_setup: hardware + netif bring-up */

#endif
