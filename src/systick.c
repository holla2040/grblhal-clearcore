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

    /* Phase 1 exit test: 1 Hz underglow blink — remove in Phase 2 */
    if ((tick_ms & 0x1FF) == 0) {       /* every 512 ms */
        sr_toggle(SR_UNDERGLOW);
    }

    sr_refresh();
}

void systick_init(void)
{
    SysTick_Config(F_CPU_HZ / 1000);            /* 1 ms */
    NVIC_SetPriority(SysTick_IRQn, 7);          /* lowest — housekeeping only */
}
