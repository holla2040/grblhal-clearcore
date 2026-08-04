/*
 * stepper.h — two-timer step engine (TC4+TC5 32-bit segment timer,
 * TC6 pulse one-shot, both prio 0).
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef __STEPPER_H__
#define __STEPPER_H__

#include "grbl/hal.h"

void stepper_claim_hal (void);                      /* set hal.stepper.* pointers */
void stepper_hw_init (void);                        /* GCLK2, TC4/5, TC6, GPIO — call from driver_setup */
void stepper_settings_changed (settings_t *settings);   /* pulse width/delay/invert */

#endif
