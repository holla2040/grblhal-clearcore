/*
 * debug_uart.h — debug console on IO-0 (combined I/O header pin 12,
 * ~1.5k load to GND for 3.3V logic; SERCOM1/PA00, 115200 TX-only).
 * Sprinkle dbg_puts()/dbg_hex32()/dbg_dec() freely for feature debugging:
 * with DEBUG_CONSOLE_ENABLE 0 they compile to nothing.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */
#ifndef DEBUG_UART_H
#define DEBUG_UART_H
#include <stdint.h>

#if DEBUG_CONSOLE_ENABLE
void dbg_init(void);
void dbg_putc(char c);
void dbg_puts(const char *s);
void dbg_hex32(uint32_t v);
void dbg_dec(uint32_t v);
#else
static inline void dbg_init(void) {}
static inline void dbg_putc(char c) { (void)c; }
static inline void dbg_puts(const char *s) { (void)s; }
static inline void dbg_hex32(uint32_t v) { (void)v; }
static inline void dbg_dec(uint32_t v) { (void)v; }
#endif

#endif
