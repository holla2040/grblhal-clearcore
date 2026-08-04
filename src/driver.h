/*
 * driver.h — grblHAL driver for Teknic ClearCore (ATSAME53N19A).
 * NOTE: do NOT change configuration here — edit my_machine.h instead.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef __DRIVER_H__
#define __DRIVER_H__

#include "clearcore.h"

#include "grbl/hal.h"

#ifndef OVERRIDE_MY_MACHINE
#include "my_machine.h"
#endif

#include "grbl/driver_opts.h"

#ifndef BAUD_RATE
#define BAUD_RATE 115200
#endif

#endif /* __DRIVER_H__ */
