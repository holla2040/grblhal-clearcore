/*
 * serial.c — COM-0 console (SERCOM7 UART) as a grblHAL io_stream_t.
 *
 * Supersedes the Phase 1 uart.c: same hardware setup (TX PB21 = PAD0,
 * RX PB20 = PAD1, pmux D, GCLK0 @ 120 MHz, 16x arithmetic baud), rewritten
 * around the core's stream buffer types with real-time command interception
 * in the RX ISR and real rx_free accounting. Stream registration pattern
 * follows the grblHAL SAM3X8E reference driver (GPLv3, Terje Io).
 * NOTE: the port is dead until the shift register selects TTL UART mode —
 * sr_init() runs first and holds the power-on default.
 *
 * NVIC: SERCOM7 DRE/RXC at priority 4 (PLAN.md map: comms = 4+).
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "driver.h"
#include "serial.h"

#include "grbl/protocol.h"

static stream_rx_buffer_t rxbuf = {0};
static stream_tx_buffer_t txbuf = {0};
static enqueue_realtime_command_ptr enqueue_realtime_command = protocol_enqueue_realtime_command;

static io_stream_properties_t serial[1];

static void sercom7_configure (uint32_t baud)
{
    SercomUsart *u = &SERCOM7->USART;

    u->CTRLA.bit.ENABLE = 0;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_ENABLE);

    /* Internal clock, 16x arithmetic oversampling, async, LSB first,
       RX on PAD1, TX on PAD0, 8N1 */
    u->CTRLA.reg = SERCOM_USART_CTRLA_MODE(0x1) |
                   SERCOM_USART_CTRLA_SAMPR(0)  |
                   SERCOM_USART_CTRLA_DORD      |
                   SERCOM_USART_CTRLA_RXPO(0x1) |
                   SERCOM_USART_CTRLA_TXPO(0x0);
    u->CTRLB.reg = SERCOM_USART_CTRLB_CHSIZE(0) |
                   SERCOM_USART_CTRLB_RXEN      |
                   SERCOM_USART_CTRLB_TXEN;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_CTRLB);

    u->BAUD.reg = (uint16_t)(65536UL -
                  (uint16_t)(((uint64_t)65536 * 16 * baud) / F_CPU_HZ));

    u->INTENSET.reg = SERCOM_USART_INTENSET_RXC;

    u->CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(u, SERCOM_USART_SYNCBUSY_ENABLE);
}

static uint16_t serialTxCount (void)
{
    uint16_t tail = txbuf.tail;

    return BUFCOUNT(txbuf.head, tail, TX_BUFFER_SIZE) +
            (SERCOM7->USART.INTFLAG.bit.TXC ? 0 : 1);
}

static uint16_t serialRxCount (void)
{
    uint16_t tail = rxbuf.tail, head = rxbuf.head;

    return BUFCOUNT(head, tail, RX_BUFFER_SIZE);
}

static uint16_t serialRxFree (void)
{
    return (RX_BUFFER_SIZE - 1) - serialRxCount();
}

static void serialRxFlush (void)
{
    rxbuf.tail = rxbuf.head;
}

static void serialRxCancel (void)
{
    rxbuf.data[rxbuf.head] = ASCII_CAN;
    rxbuf.tail = rxbuf.head;
    rxbuf.head = BUFNEXT(rxbuf.head, rxbuf);
}

static int32_t serialGetC (void)
{
    uint_fast16_t tail = rxbuf.tail;

    if(tail == rxbuf.head)
        return -1;

    char data = rxbuf.data[tail];
    rxbuf.tail = BUFNEXT(tail, rxbuf);

    return (int32_t)data;
}

static bool serialPutC (const char c)
{
    uint_fast16_t next_head = BUFNEXT(txbuf.head, txbuf);

    while(txbuf.tail == next_head) {                    // Buffer full: block,
        if(!hal.stream_blocking_callback())             // giving the core a
            return false;                               // chance to abort.
    }

    txbuf.data[txbuf.head] = c;
    txbuf.head = next_head;

    SERCOM7->USART.INTENSET.reg = SERCOM_USART_INTENSET_DRE;

    return true;
}

static void serialWriteS (const char *s)
{
    char c, *ptr = (char *)s;

    while((c = *ptr++) != '\0')
        serialPutC(c);
}

static void serialWrite (const char *s, uint16_t length)
{
    char *ptr = (char *)s;

    while(length--)
        serialPutC(*ptr++);
}

static bool serialSuspendInput (bool suspend)
{
    return stream_rx_suspend(&rxbuf, suspend);
}

static bool serialSetBaudRate (uint32_t baud_rate)
{
    sercom7_configure(baud_rate);

    return true;
}

static bool serialDisable (bool disable)
{
    if(disable)
        SERCOM7->USART.INTENCLR.reg = SERCOM_USART_INTENCLR_RXC;
    else
        SERCOM7->USART.INTENSET.reg = SERCOM_USART_INTENSET_RXC;

    return true;
}

static bool serialEnqueueRtCommand (char c)
{
    return enqueue_realtime_command(c);
}

static enqueue_realtime_command_ptr serialSetRtHandler (enqueue_realtime_command_ptr handler)
{
    enqueue_realtime_command_ptr prev = enqueue_realtime_command;

    if(handler)
        enqueue_realtime_command = handler;

    return prev;
}

static const io_stream_t *serialInit (uint32_t baud_rate)
{
    static const io_stream_t stream = {
        .type = StreamType_Serial,
        .is_connected = stream_connected,
        .read = serialGetC,
        .write = serialWriteS,
        .write_n = serialWrite,
        .write_char = serialPutC,
        .enqueue_rt_command = serialEnqueueRtCommand,
        .get_rx_buffer_count = serialRxCount,
        .get_tx_buffer_count = serialTxCount,
        .get_rx_buffer_free = serialRxFree,
        .reset_read_buffer = serialRxFlush,
        .cancel_read_buffer = serialRxCancel,
        .disable_rx = serialDisable,
        .suspend_read = serialSuspendInput,
        .set_baud_rate = serialSetBaudRate,
        .set_enqueue_rt_handler = serialSetRtHandler
    };

    if(serial[0].flags.claimed)
        return NULL;

    serial[0].flags.claimed = On;

    if(!serial[0].flags.init_ok) {

        CLOCK_ENABLE(APBDMASK, SERCOM7_);
        SET_CLOCK_SOURCE(SERCOM7_GCLK_ID_CORE, 0);      /* GCLK0 = 120 MHz */

        pin_pmux(COM0_TX_GRP, COM0_TX_PIN, COM0_PMUX_FUNC);
        pin_pmux(COM0_RX_GRP, COM0_RX_PIN, COM0_PMUX_FUNC);

        SERCOM7->USART.CTRLA.bit.SWRST = 1;
        SYNCBUSY_WAIT(&SERCOM7->USART, SERCOM_USART_SYNCBUSY_SWRST);

        NVIC_SetPriority(SERCOM7_0_IRQn, 4);            /* DRE */
        NVIC_SetPriority(SERCOM7_2_IRQn, 4);            /* RXC */
        NVIC_EnableIRQ(SERCOM7_0_IRQn);
        NVIC_EnableIRQ(SERCOM7_2_IRQn);

        serial[0].flags.init_ok = On;
    }

    sercom7_configure(baud_rate);

    return &stream;
}

void serialRegisterStreams (void)
{
    static io_stream_details_t streams = {
        .n_streams = 1,
        .streams = serial,
    };

    serial[0].type = StreamType_Serial;
    serial[0].instance = 0;
    serial[0].flags.claimable = On;
    serial[0].flags.claimed = Off;
    serial[0].flags.can_set_baud = On;
    serial[0].claim = serialInit;

    stream_register_streams(&streams);
}

/* SERCOM7 DRE — TX data register empty */
void SERCOM7_0_Handler (void)
{
    uint_fast16_t tail = txbuf.tail;

    if(tail != txbuf.head) {
        SERCOM7->USART.DATA.reg = (uint8_t)txbuf.data[tail];
        txbuf.tail = tail = BUFNEXT(tail, txbuf);
    }

    if(tail == txbuf.head)
        SERCOM7->USART.INTENCLR.reg = SERCOM_USART_INTENCLR_DRE;
}

/* SERCOM7 RXC — receive complete */
void SERCOM7_2_Handler (void)
{
    char data = (char)SERCOM7->USART.DATA.reg;          /* read clears RXC */

    if(!enqueue_realtime_command(data)) {
        uint_fast16_t next_head = BUFNEXT(rxbuf.head, rxbuf);
        if(next_head == rxbuf.tail)
            rxbuf.overflow = 1;
        else {
            rxbuf.data[rxbuf.head] = data;
            rxbuf.head = next_head;
        }
    }
}
