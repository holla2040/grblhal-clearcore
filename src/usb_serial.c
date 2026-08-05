/*
 * usb_serial.c — TinyUSB CDC-ACM as a grblHAL io_stream_t (USB target
 * port, DM = PA24, DP = PA25, mux H). The Prime Directive holds: TinyUSB
 * is OUR dependency, configured and serviced only from contexts we wrote —
 * USB IRQs at prio 4 feed TinyUSB's event queue, tud_task() runs from the
 * grbl foreground (on_execute_realtime chain + blocking-write loops).
 *
 * 1200-baud touch: setting the CDC line coding to 1200 baud writes the
 * UF2 bootloader's double-tap magic to the top of RAM and resets — the
 * standard SAMD "click to bootloader" flow (hardware double-tap remains
 * the fallback).
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "driver.h"
#include "usb_serial.h"
#include "debug_uart.h"

#include "tusb.h"

#include "grbl/protocol.h"

/* UF2 SAMD bootloader double-tap magic, at the last word of RAM */
#define DBL_TAP_MAGIC 0xf01669efUL
#define DBL_TAP_PTR   ((volatile uint32_t *)(0x20000000UL + 0x30000UL - 4))

static stream_rx_buffer_t rxbuf = {0};
static enqueue_realtime_command_ptr enqueue_realtime_command = protocol_enqueue_realtime_command;

/* Pump TinyUSB and drain CDC RX into our ring, intercepting RT commands */
static void usb_pump (void)
{
    tud_task();

    while(tud_cdc_available()) {
        char data = (char)tud_cdc_read_char();

        dbg_putc(data);     /* mirror inbound USB to the debug console:
                               shows the command in flight when a hang hits */

        /* local echo so a bare terminal shows what was typed */
        if(data == '\r' || data == '\n') {
            tud_cdc_write_char('\r');
            tud_cdc_write_char('\n');
        } else
            tud_cdc_write_char(data);
        tud_cdc_write_flush();

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
}

void usb_poll (void)
{
    usb_pump();
}

static bool usbIsConnected (void)
{
    return tud_cdc_connected();
}

static uint16_t usbRxCount (void)
{
    uint16_t tail = rxbuf.tail, head = rxbuf.head;
    return BUFCOUNT(head, tail, RX_BUFFER_SIZE);
}

static uint16_t usbRxFree (void)
{
    return (RX_BUFFER_SIZE - 1) - usbRxCount();
}

static void usbRxFlush (void)
{
    rxbuf.tail = rxbuf.head;
}

static void usbRxCancel (void)
{
    rxbuf.data[rxbuf.head] = ASCII_CAN;
    rxbuf.tail = rxbuf.head;
    rxbuf.head = BUFNEXT(rxbuf.head, rxbuf);
}

static int32_t usbGetC (void)
{
    uint_fast16_t tail = rxbuf.tail;

    if(tail == rxbuf.head)
        return -1;

    char data = rxbuf.data[tail];
    rxbuf.tail = BUFNEXT(tail, rxbuf);

    return (int32_t)data;
}

static bool usbPutC (const char c)
{
    if(!tud_cdc_connected())
        return true;                        /* discard when no host */

    while(tud_cdc_write_char(c) == 0) {
        tud_cdc_write_flush();
        usb_pump();
        if(!hal.stream_blocking_callback())
            return false;
        if(!tud_cdc_connected())
            return true;
    }

    if(c == ASCII_LF)
        tud_cdc_write_flush();

    return true;
}

static void usbWriteS (const char *s)
{
    char c, *ptr = (char *)s;

    while((c = *ptr++) != '\0')
        usbPutC(c);
}

static void usbWrite (const char *s, uint16_t length)
{
    char *ptr = (char *)s;

    while(length--)
        usbPutC(*ptr++);
}

static uint16_t usbTxCount (void)
{
    return (uint16_t)(CFG_TUD_CDC_TX_BUFSIZE - tud_cdc_write_available());
}

static bool usbSuspendInput (bool suspend)
{
    return stream_rx_suspend(&rxbuf, suspend);
}

static bool usbEnqueueRtCommand (char c)
{
    return enqueue_realtime_command(c);
}

static enqueue_realtime_command_ptr usbSetRtHandler (enqueue_realtime_command_ptr handler)
{
    enqueue_realtime_command_ptr prev = enqueue_realtime_command;

    if(handler)
        enqueue_realtime_command = handler;

    return prev;
}

const io_stream_t *usb_serialInit (void)
{
    static const io_stream_t stream = {
        .type = StreamType_Serial,
        .is_connected = usbIsConnected,
        .read = usbGetC,
        .write = usbWriteS,
        .write_n = usbWrite,
        .write_char = usbPutC,
        .enqueue_rt_command = usbEnqueueRtCommand,
        .get_rx_buffer_count = usbRxCount,
        .get_tx_buffer_count = usbTxCount,
        .get_rx_buffer_free = usbRxFree,
        .reset_read_buffer = usbRxFlush,
        .cancel_read_buffer = usbRxCancel,
        .suspend_read = usbSuspendInput,
        .set_enqueue_rt_handler = usbSetRtHandler
    };

    /* USB clocks: GCLK4 = 48 MHz (DPLL0), AHB+APB bus clocks */
    CLOCK_ENABLE(AHBMASK, USB_);
    CLOCK_ENABLE(APBBMASK, USB_);
    SET_CLOCK_SOURCE(USB_GCLK_ID, 4);

    pin_pmux(GRP_A, 24, 7);     /* PA24 = USB DM, mux H */
    pin_pmux(GRP_A, 25, 7);     /* PA25 = USB DP, mux H */

    NVIC_SetPriority(USB_OTHER_IRQn, 4);
    NVIC_SetPriority(USB_SOF_HSOF_IRQn, 4);
    NVIC_SetPriority(USB_TRCPT0_IRQn, 4);
    NVIC_SetPriority(USB_TRCPT1_IRQn, 4);
    NVIC_EnableIRQ(USB_OTHER_IRQn);
    NVIC_EnableIRQ(USB_SOF_HSOF_IRQn);
    NVIC_EnableIRQ(USB_TRCPT0_IRQn);
    NVIC_EnableIRQ(USB_TRCPT1_IRQn);

    tusb_init();

    return &stream;
}

/* 1200-baud touch → UF2 bootloader */
void tud_cdc_line_coding_cb (uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;

    if(coding->bit_rate == 1200) {
        *DBL_TAP_PTR = DBL_TAP_MAGIC;
        NVIC_SystemReset();
    }
}

/* USB interrupt handlers (startup_same53.c weak aliases overridden) */
void USB_OTHER_Handler (void)    { tud_int_handler(0); }
void USB_SOF_HSOF_Handler (void) { tud_int_handler(0); }
void USB_TRCPT0_Handler (void)   { tud_int_handler(0); }
void USB_TRCPT1_Handler (void)   { tud_int_handler(0); }
