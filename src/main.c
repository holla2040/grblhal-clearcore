/*
 * grblhal-clearcore — Phase 1 main: board bring-up.
 * SysTick 1 ms tick + shift-register refresh (underglow blinks at ~1 Hz),
 * COM-0 UART console echoes at 115200-8N1. Replaced by the grblHAL driver
 * entry in Phase 2.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "clearcore.h"
#include "shiftreg.h"
#include "systick.h"
#include "uart.h"

int main(void)
{
    sr_init();              /* first: COM-0 is dead until SR sets TTL UART mode */
    systick_init();
    uart_init(115200);

    uart_write("\r\ngrblhal-clearcore phase 1 — echo console\r\n");

    for (;;) {
        int c = uart_getc();
        if (c >= 0) {
            uart_putc((char)c);
        }
    }
}
