/* debug_uart.h — TEMP DIAG console (XBee header, SERCOM2, 3.3 V, 115200). */
#ifndef DEBUG_UART_H
#define DEBUG_UART_H
#include <stdint.h>
void dbg_init(void);
void dbg_putc(char c);
void dbg_puts(const char *s);
void dbg_hex32(uint32_t v);
void dbg_dec(uint32_t v);
#endif
