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
#include "debug_uart.h"

static volatile uint32_t tick_ms;

void (*systick_hook)(void);     /* optional per-ms callback (grblHAL delay) */

/* Boot telemetry: boot_stage slow countable blinks (250 ms on, 750 ms
   off), then a 3 s pause, repeat.
   1 driver_init running, 2 driver_init done, 3 nvsRead in, 4 nvsRead out,
   5 spindleConfig in, 6 spindleConfig out, 7 nvsWrite in, 8 erase done,
   9 nvsWrite out, 10 driver_setup in, 11 driver_setup out, 12 main loop. */
volatile uint8_t boot_stage = 1;

uint32_t millis(void)
{
    return tick_ms;
}

void delay_ms(uint32_t ms)
{
    uint32_t start = tick_ms;
    while ((tick_ms - start) < ms) { }
}

/* --- TEMP DIAG: wedge PC capture ---------------------------------------
 * The naked handler grabs the interrupted thread's stacked PC/LR, then the
 * C body runs as usual. If boot_stage hasn't advanced for 5 s, the PC/LR
 * are quad-word-written to the (erased) NVS block at 0x7E000, where the
 * bootloader's CURRENT.UF2 exposes them over USB. REVERT BEFORE COMMIT. */

void systick_body(uint32_t pc, uint32_t lr);

__attribute__((naked)) void SysTick_Handler(void)
{
    __asm volatile (
        "mrs r0, msp        \n"
        "ldr r1, [r0, #20]  \n"    /* stacked LR */
        "ldr r0, [r0, #24]  \n"    /* stacked PC */
        "b   systick_body   \n");
}

static void wedge_dump(uint32_t pc, uint32_t lr)
{
    volatile uint32_t *dst = (volatile uint32_t *)0x7FFD0UL;

    if (dst[0] != 0xFFFFFFFFUL)     /* target not erased: never risk an
                                       NVMCTRL error-spin inside the ISR */
        return;

    while(!NVMCTRL->STATUS.bit.READY);
    NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMD_PBC | NVMCTRL_CTRLB_CMDEX_KEY;
    while(!NVMCTRL->STATUS.bit.READY);

    dst[0] = 0xDEADBEEF;
    dst[1] = pc;
    dst[2] = lr;
    dst[3] = boot_stage;

    NVMCTRL->ADDR.reg = 0x7FFD0UL;
    NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMD_WQW | NVMCTRL_CTRLB_CMDEX_KEY;
    while(!NVMCTRL->STATUS.bit.READY);
}

void systick_body(uint32_t pc, uint32_t lr)
{
    static uint8_t last_stage = 0;
    static uint16_t stuck_ms = 0;
    static bool dumped = false;

    tick_ms++;

    if (boot_stage != last_stage) {
        dbg_puts("stage ");
        dbg_dec(boot_stage);
        dbg_puts("\n");
        last_stage = boot_stage;
        stuck_ms = 0;
    } else if (!dumped && boot_stage != 11 && ++stuck_ms >= 5000) {
        dumped = true;              /* stage 11 = healthy terminal state */
        wedge_dump(pc, lr);
    }

    /* Underglow: slow counted-blink telemetry (see boot_stage above) */
    {
        static uint16_t phase_ms = 2900;    /* first burst ~100 ms after boot */
        static uint8_t blinks_left = 0;

        phase_ms++;
        if (blinks_left == 0) {
            if (phase_ms >= 3000) {
                blinks_left = boot_stage;
                phase_ms = 0;
            }
        } else {
            if (phase_ms == 1)
                sr_set(SR_UNDERGLOW);
            else if (phase_ms == 250)
                sr_clear(SR_UNDERGLOW);
            else if (phase_ms >= 1000) {
                phase_ms = 0;
                blinks_left--;
            }
        }
    }

    if (systick_hook) {
        systick_hook();
    }

    sr_refresh();
}

/* TEMP DIAG: watchdog early-warning sampler at NVIC prio 0 — preempts any
   interrupt storm at prio >=1 and records the interrupted context's
   PC/LR/xPSR (xPSR[8:0] = which handler was hogging; 0 = thread mode).
   Slot 0x7FFC0. Feeds the WDT so the board limps on. REVERT BEFORE COMMIT. */

void wdt_body(uint32_t pc, uint32_t lr, uint32_t psr);
static uint16_t gclk5_probe_read(void);

__attribute__((naked)) void WDT_Handler(void)
{
    __asm volatile (
        "mrs r0, msp        \n"
        "ldr r2, [r0, #28]  \n"    /* stacked xPSR */
        "ldr r1, [r0, #20]  \n"    /* stacked LR */
        "ldr r0, [r0, #24]  \n"    /* stacked PC */
        "b   wdt_body       \n");
}

void wdt_body(uint32_t pc, uint32_t lr, uint32_t psr)
{
    static uint8_t ew_count = 0;
    volatile uint32_t *dst = (volatile uint32_t *)0x7FF00UL;

    WDT->INTFLAG.reg = WDT_INTFLAG_EW;
    WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;       /* feed: no reset */

    dbg_puts("wdt ew#");
    dbg_dec(ew_count);
    dbg_puts(" tick_ms=");
    dbg_dec(tick_ms);
    dbg_puts(" gclk5=");
    dbg_dec(gclk5_probe_read());
    dbg_puts(" stage=");
    dbg_dec(boot_stage);
    dbg_puts(" pc=");
    dbg_hex32(pc);
    dbg_puts("\n");

    if (++ew_count < 4)                          /* sample at steady state */
        return;

    if (dst[0] == 0xFFFFFFFFUL) {
        while(!NVMCTRL->STATUS.bit.READY);
        NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMD_PBC | NVMCTRL_CTRLB_CMDEX_KEY;
        while(!NVMCTRL->STATUS.bit.READY);
        dst[0] = 0xCAFED00D;
        dst[1] = tick_ms;           /* ~500 if CPU at 120 MHz */
        dst[2] = gclk5_probe_read();/* ~7812 if XOSC1/GCLK5 alive (15625 Hz) */
        dst[3] = boot_stage;
        NVMCTRL->ADDR.reg = 0x7FF00UL;
        NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMD_WQW | NVMCTRL_CTRLB_CMDEX_KEY;
        while(!NVMCTRL->STATUS.bit.READY);
    }
}

static void gclk5_probe_init(void)
{
    /* TC1 free-running from GCLK5 (1 MHz, XOSC1-derived), DIV64 = 15625 Hz.
       If XOSC1/GCLK5 are dead the counter stays at 0. */
    CLOCK_ENABLE(APBAMASK, TC1_);
    SET_CLOCK_SOURCE(TC1_GCLK_ID, 5);
    TC1->COUNT16.CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(&TC1->COUNT16, TC_SYNCBUSY_SWRST);
    TC1->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV64;
    TC1->COUNT16.CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(&TC1->COUNT16, TC_SYNCBUSY_ENABLE);
}

static uint16_t gclk5_probe_read(void)
{
    TC1->COUNT16.CTRLBSET.reg = TC_CTRLBSET_CMD_READSYNC;
    while(TC1->COUNT16.CTRLBSET.bit.CMD);
    return TC1->COUNT16.COUNT.reg;
}

static void wdt_sampler_init(void)
{
    WDT->CTRLA.reg = 0;
    while(WDT->SYNCBUSY.reg);
    WDT->CONFIG.reg = WDT_CONFIG_PER(0xB);          /* ~16 s timeout */
    WDT->EWCTRL.reg = WDT_EWCTRL_EWOFFSET(0x6);     /* EW @ ~0.5 s */
    WDT->INTFLAG.reg = WDT_INTFLAG_EW;          /* drop any stale flag */
    WDT->INTENSET.reg = WDT_INTENSET_EW;
    NVIC_SetPriority(WDT_IRQn, 0);
    NVIC_EnableIRQ(WDT_IRQn);
    WDT->CTRLA.reg = WDT_CTRLA_ENABLE;
    while(WDT->SYNCBUSY.reg);
}

void systick_init(void)
{
    gclk5_probe_init();
    wdt_sampler_init();
    SysTick_Config(F_CPU_HZ / 1000);            /* 1 ms */
    NVIC_SetPriority(SysTick_IRQn, 7);          /* lowest — housekeeping only */
}
