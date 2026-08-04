/*
 * systick.c — our 1 ms tick (PLAN.md NVIC map: SysTick = priority 7).
 * Housekeeping per tick: shift-register refresh. Future grblHAL
 * hal.get_elapsed_ticks reads millis().
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "clearcore.h"
#include "shiftreg.h"
#include "systick.h"

static volatile uint32_t tick_ms;

void (*systick_hook)(void);     /* optional per-ms callback (grblHAL delay) */

uint32_t millis(void)
{
    return tick_ms;
}

void delay_ms(uint32_t ms)
{
    uint32_t start = tick_ms;
    while ((tick_ms - start) < ms) { }
}

void SysTick_Handler(void)
{
    tick_ms++;

    /* ~1 Hz underglow blink: "our firmware is alive" indicator */
    if ((tick_ms & 0x1FF) == 0) {       /* every 512 ms */
        sr_toggle(SR_UNDERGLOW);
    }

    if (systick_hook) {
        systick_hook();
    }

    sr_refresh();
}

void systick_init(void)
{
    SysTick_Config(F_CPU_HZ / 1000);            /* 1 ms */
    NVIC_SetPriority(SysTick_IRQn, 7);          /* lowest — housekeeping only */
}
