/*
 * uart.c — COM-0 console on SERCOM7 (TX PB21 = PAD0, RX PB20 = PAD1,
 * pmux function D), interrupt-driven with TX/RX ring buffers.
 *
 * Register idioms cross-checked against Teknic SerialBase.cpp (MIT):
 * internal-clock USART from GCLK0 (120 MHz), 16x arithmetic baud
 * (BAUD = 65536*(1 - 16*baud/fref)), LSB-first, 8N1, RXPO=1/TXPO=0.
 * NOTE: the port is dead until the shift register drives COM-0 into TTL
 * UART mode (SR_UART_TTL_0 = 0, SR_UART_SPI_SEL_0 = 0) — sr_init() does
 * this; call it first.
 *
 * The COM-0 jack carries 5V TTL — use a 5V-tolerant USB-UART adapter.
 * NVIC: SERCOM7 DRE/RXC at priority 4 (PLAN.md map: comms = 4+).
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "clearcore.h"
#include "uart.h"

#define TX_BUF 256                  /* power of two */
#define RX_BUF 256

static volatile uint8_t txbuf[TX_BUF], rxbuf[RX_BUF];
static volatile uint16_t txhead, txtail, rxhead, rxtail;

void uart_init(uint32_t baud)
{
    SercomUsart *u = &SERCOM7->USART;

    CLOCK_ENABLE(APBDMASK, SERCOM7_);
    SET_CLOCK_SOURCE(SERCOM7_GCLK_ID_CORE, 0);  /* GCLK0 = 120 MHz */

    pin_pmux(COM0_TX_GRP, COM0_TX_PIN, COM0_PMUX_FUNC);
    pin_pmux(COM0_RX_GRP, COM0_RX_PIN, COM0_PMUX_FUNC);

    u->CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_SWRST);

    /* Internal clock, 16x arithmetic oversampling, async, LSB first,
       RX on PAD1, TX on PAD0, no parity */
    u->CTRLA.reg = SERCOM_USART_CTRLA_MODE(0x1) |
                   SERCOM_USART_CTRLA_SAMPR(0)  |
                   SERCOM_USART_CTRLA_DORD      |
                   SERCOM_USART_CTRLA_RXPO(0x1) |
                   SERCOM_USART_CTRLA_TXPO(0x0);
    /* 8 data bits, 1 stop bit, receiver + transmitter on */
    u->CTRLB.reg = SERCOM_USART_CTRLB_CHSIZE(0) |
                   SERCOM_USART_CTRLB_RXEN      |
                   SERCOM_USART_CTRLB_TXEN;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_CTRLB);

    u->BAUD.reg = (uint16_t)(65536UL -
                  (uint16_t)(((uint64_t)65536 * 16 * baud) / F_CPU_HZ));

    u->INTENSET.reg = SERCOM_USART_INTENSET_RXC;

    NVIC_SetPriority(SERCOM7_0_IRQn, 4);        /* DRE */
    NVIC_SetPriority(SERCOM7_2_IRQn, 4);        /* RXC */
    NVIC_EnableIRQ(SERCOM7_0_IRQn);
    NVIC_EnableIRQ(SERCOM7_2_IRQn);

    u->CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_ENABLE);
}

void uart_putc(char c)
{
    uint16_t next = (txhead + 1) & (TX_BUF - 1);

    while (next == txtail) { }                  /* ring full: wait for DRE ISR */

    txbuf[txhead] = (uint8_t)c;
    txhead = next;
    SERCOM7->USART.INTENSET.reg = SERCOM_USART_INTENSET_DRE;
}

void uart_write(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

int uart_getc(void)
{
    int c;

    if (rxhead == rxtail) {
        return -1;
    }
    c = rxbuf[rxtail];
    rxtail = (rxtail + 1) & (RX_BUF - 1);
    return c;
}

uint16_t uart_rx_free(void)
{
    return (uint16_t)(RX_BUF - 1 - ((rxhead - rxtail) & (RX_BUF - 1)));
}

/* TX data-register-empty */
void SERCOM7_0_Handler(void)
{
    if (txhead == txtail) {
        SERCOM7->USART.INTENCLR.reg = SERCOM_USART_INTENCLR_DRE;
        return;
    }
    SERCOM7->USART.DATA.reg = txbuf[txtail];
    txtail = (txtail + 1) & (TX_BUF - 1);
}

/* RX complete */
void SERCOM7_2_Handler(void)
{
    uint8_t c = (uint8_t)SERCOM7->USART.DATA.reg;   /* reading clears RXC */
    uint16_t next = (rxhead + 1) & (RX_BUF - 1);

    if (next != rxtail) {                           /* drop char if ring full */
        rxbuf[rxhead] = c;
        rxhead = next;
    }
}
