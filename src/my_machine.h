/*
 * my_machine.h — build configuration for grblhal-clearcore.
 * Force-included into EVERY compilation unit (core + driver) via
 * `-include src/my_machine.h` in platformio.ini, so core and driver always
 * agree on N_AXIS and option flags. Do not include it manually.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef MY_MACHINE_H
#define MY_MACHINE_H

#define N_AXIS 4

// HLFB servo-fault monitor: enable (set to 1) ONLY after bench-verifying
// HLFB polarity with a configured ClearPath (MSP "Servo On" HLFB mode) —
// otherwise unconfigured motors alarm immediately.
#define HLFB_MONITOR_ENABLE 0

// Plugins (all off; networking/sdcard arrive in Phases 6/7)

#endif
