/*
 * uart.h — COM-0 console (SERCOM7 UART, RJ-45 jack, 5V TTL, 115200-8N1).
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(uint32_t baud);
void uart_putc(char c);              /* blocks briefly if TX ring full */
void uart_write(const char *s);
int uart_getc(void);                 /* -1 if RX ring empty */
uint16_t uart_rx_free(void);         /* real free-space accounting */

#endif /* UART_H */
