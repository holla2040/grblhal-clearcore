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

// Input polarity defaults (schematic netlist 2026-08-04): every ClearCore
// input is electrically non-inverting but industrially ACTIVE-LOW — a
// closed switch / conducting NPN sensor pulls the pin low. Invert all.
#define DEFAULT_LIMIT_SIGNALS_INVERT_MASK AXES_BITMASK
#define DEFAULT_CONTROL_SIGNALS_INVERT_MASK SIGNALS_BITMASK
#define DEFAULT_PROBE_SIGNAL_INVERT On

// USB CDC console (TinyUSB): primary stream when connected, UART stays
// registered as an instance. Comment out to go UART-only.
#define USB_SERIAL_CDC 1

// Ethernet (SAME53 GMAC + lwIP + networking plugin), telnet first
#define ETHERNET_ENABLE 1
#define TELNET_ENABLE 1
#define WEBSOCKET_ENABLE 1

// SD card job streaming (SERCOM4 SPI + FatFs + sdcard plugin)
#define SDCARD_ENABLE 1

#endif
