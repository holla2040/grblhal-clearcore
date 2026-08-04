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

// Plugins (all off in Phase 2; networking/sdcard arrive in Phases 6/7)

#endif
