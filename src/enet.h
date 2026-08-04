/*
 * enet.h — Ethernet + networking services for grblhal-clearcore.
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef __ENET_H__
#define __ENET_H__

#include <stdbool.h>

bool grbl_enet_start (void);    /* register settings + bring up when configured */

#endif
