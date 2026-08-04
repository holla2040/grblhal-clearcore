/*
 * debug_uart.c — TEMP DIAG bring-up console on IO-0 via the COMBINED I/O
 * header (pin 12). MCU TX = PA00 = SERCOM1 PAD0 (mux D). The board's
 * XOR-inverter + low-side FET double-invert, so the connector carries
 * standard-polarity UART as 0<->24 V open-drain (internal 10 k pull-up).
 * Load IO-0 with ~1.5 kOhm to GND to divide the high level to ~3.1 V for a
 * 3.3 V listener. 115200-8N1, TX-only, blocking, no interrupts.
 * NOTE: claims IO-0 — coolant mist output is unavailable while active.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "clearcore.h"
#include "debug_uart.h"

void dbg_init(void)
{
    SercomUsart *u = &SERCOM1->USART;

    CLOCK_ENABLE(APBAMASK, SERCOM1_);
    SET_CLOCK_SOURCE(SERCOM1_GCLK_ID_CORE, 0);      /* GCLK0 = 120 MHz */

    pin_pmux(GRP_A, 0, 3);      /* PA00 = SERCOM1 PAD0 = TX, mux D */

    u->CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_SWRST);

    u->CTRLA.reg = SERCOM_USART_CTRLA_MODE(0x1) |
                   SERCOM_USART_CTRLA_SAMPR(0)  |
                   SERCOM_USART_CTRLA_DORD      |
                   SERCOM_USART_CTRLA_RXPO(0x1) |
                   SERCOM_USART_CTRLA_TXPO(0x0);
    u->CTRLB.reg = SERCOM_USART_CTRLB_CHSIZE(0) |
                   SERCOM_USART_CTRLB_TXEN;     /* TX-only */
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_CTRLB);

    u->BAUD.reg = (uint16_t)(65536UL -
                  (uint16_t)(((uint64_t)65536 * 16 * 115200UL) / F_CPU_HZ));

    u->CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_ENABLE);
}

void dbg_putc(char c)
{
    SercomUsart *u = &SERCOM1->USART;

    while (!u->INTFLAG.bit.DRE) { }
    u->DATA.reg = (uint8_t)c;
}

void dbg_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            dbg_putc('\r');
        dbg_putc(*s++);
    }
}

void dbg_hex32(uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        dbg_putc(h[(v >> i) & 0xF]);
}

void dbg_dec(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    do { buf[--i] = '0' + v % 10; v /= 10; } while (v && i);
    dbg_puts(&buf[i]);
}
