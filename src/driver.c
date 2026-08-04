/*
 * driver.c — grblHAL driver for Teknic ClearCore (ATSAME53N19A, bare metal).
 *
 * Phase 2 scope: boot the core against HAL v10 — console stream, flash NVS,
 * 1 ms tick, real motor-enable control via the shift register. Stepper
 * motion, limits, probe and spindle arrive in Phases 3/4; their handlers
 * here are safe no-ops so the core can boot and be configured.
 * Structure follows the grblHAL SAM3X8E reference driver (GPLv3, Terje Io).
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include <string.h>

#include "driver.h"
#include "serial.h"
#include "shiftreg.h"
#include "systick.h"

#include "grbl/grbllib.h"
#include "grbl/protocol.h"
#include "grbl/settings.h"
#include "grbl/nvs_buffer.h"
#include "grbl/state_machine.h"
#include "grbl/stream.h"

static bool IOInitDone = false;
static coolant_state_t coolant_state = {0};
static delay_t grbl_delay = { .ms = 0, .callback = NULL };

/* --- 1 ms delay (hal.delay_ms), serviced from our SysTick hook --- */

static void systick_hook_fn (void)
{
    if(grbl_delay.ms && !(--grbl_delay.ms)) {
        if(grbl_delay.callback) {
            grbl_delay.callback();
            grbl_delay.callback = NULL;
        }
    }
}

static void driver_delay_ms (uint32_t ms, delay_callback_ptr callback)
{
    if(ms) {
        grbl_delay.ms = ms;
        if(!(grbl_delay.callback = callback)) {
            while(grbl_delay.ms);
        }
    } else {
        if(grbl_delay.ms) {
            grbl_delay.callback = NULL;
            grbl_delay.ms = 1;
        }
        if(callback)
            callback();
    }
}

/* --- Steppers: enables are REAL (shift-register bits), motion is Phase 3 --- */

static void stepperEnable (axes_signals_t enable, bool hold)
{
    static const uint32_t en_bit[] = { SR_EN_OUT_0, SR_EN_OUT_1, SR_EN_OUT_2, SR_EN_OUT_3 };

    (void)hold;

    enable.value ^= settings.steppers.enable_invert.mask;

    uint_fast8_t motor;
    for(motor = 0; motor < N_AXIS; motor++) {
        if(enable.value & (1UL << motor))
            sr_set(en_bit[motor]);
        else
            sr_clear(en_bit[motor]);
    }
}

/* Phase 3 replaces these no-ops with the TC4/TC5 + TC6 two-timer engine. */
static void stepperWakeUp (void)
{
    stepperEnable((axes_signals_t){AXES_BITMASK}, false);
}

static void stepperGoIdle (bool clear_signals)
{
    (void)clear_signals;
}

static void stepperCyclesPerTick (uint32_t cycles_per_tick)
{
    (void)cycles_per_tick;
}

static void stepperPulseStart (stepper_t *stepper)
{
    (void)stepper;
}

/* --- Limits / control: no inputs wired until Phase 3 --- */

static void limitsEnable (bool on, axes_signals_t homing_cycle)
{
    (void)on; (void)homing_cycle;
}

static limit_signals_t limitsGetState (void)
{
    limit_signals_t signals = {0};

    signals.min.mask = settings.limits.invert.mask;
    if(settings.limits.invert.mask)
        signals.min.value ^= settings.limits.invert.mask;

    return signals;
}

static control_signals_t systemGetState (void)
{
    control_signals_t signals = { settings.control_invert.value };

    if(settings.control_invert.value)
        signals.value ^= settings.control_invert.value;

    return signals;
}

/* --- Coolant: state is tracked, outputs arrive in Phase 4 --- */

static void coolantSetState (coolant_state_t mode)
{
    coolant_state = mode;
}

static coolant_state_t coolantGetState (void)
{
    return coolant_state;
}

/* --- Atomic helpers (ported from the 2021 driver.c:157-177) --- */

static void bitsSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    __disable_irq();
    *ptr |= bits;
    __enable_irq();
}

static uint_fast16_t bitsClearAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    __disable_irq();
    uint_fast16_t prev = *ptr;
    *ptr &= ~bits;
    __enable_irq();
    return prev;
}

static uint_fast16_t valueSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t value)
{
    __disable_irq();
    uint_fast16_t prev = *ptr;
    *ptr = value;
    __enable_irq();
    return prev;
}

/* --- NVS: grblHAL settings in the last 8 KB flash block ---
 *
 * The linker script shortens FLASH so nothing else lands here. NVMCTRL on
 * SAME5x: erase granularity = 8 KB block, write granularity = 512 B page
 * via the page buffer. NVS_SIZE (2048) = exactly 4 pages.
 */

#define NVS_FLASH_ADDR 0x7E000UL

static bool nvsRead (uint8_t *dest)
{
    memcpy(dest, (void *)NVS_FLASH_ADDR, NVS_SIZE);

    return true;
}

static bool nvsWrite (uint8_t *source)
{
    uint32_t *src = (uint32_t *)source;
    uint32_t page_addr = NVS_FLASH_ADDR;
    uint_fast8_t pages = NVS_SIZE / NVMCTRL_PAGE_SIZE;

    while(!NVMCTRL->STATUS.bit.READY);

    /* Erase the block */
    NVMCTRL->INTFLAG.reg = NVMCTRL_INTFLAG_DONE;
    NVMCTRL->ADDR.reg = NVS_FLASH_ADDR;
    NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMD_EB | NVMCTRL_CTRLB_CMDEX_KEY;
    while(!NVMCTRL->STATUS.bit.READY);

    /* Write pages: clear page buffer, fill via 32-bit writes, commit */
    while(pages--) {
        NVMCTRL->INTFLAG.reg = NVMCTRL_INTFLAG_DONE;
        NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMD_PBC | NVMCTRL_CTRLB_CMDEX_KEY;
        while(!NVMCTRL->STATUS.bit.READY);

        volatile uint32_t *dst = (volatile uint32_t *)page_addr;
        uint_fast16_t i = NVMCTRL_PAGE_SIZE / sizeof(uint32_t);
        while(i--)
            *dst++ = *src++;

        NVMCTRL->INTFLAG.reg = NVMCTRL_INTFLAG_DONE;
        NVMCTRL->ADDR.reg = page_addr;
        NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMD_WP | NVMCTRL_CTRLB_CMDEX_KEY;
        while(!NVMCTRL->STATUS.bit.READY);

        page_addr += NVMCTRL_PAGE_SIZE;
    }

    /* Invalidate the CMCC cache so reads see the new data */
    CMCC->CTRL.reg = 0;
    while(CMCC->SR.bit.CSTS);
    CMCC->MAINT0.reg = CMCC_MAINT0_INVALL;
    CMCC->CTRL.reg = CMCC_CTRL_CEN;

    return true;
}

/* --- Settings hooks --- */

static void settings_changed (settings_t *settings, settings_changed_flags_t changed)
{
    /* Nothing to reconfigure yet — the core applies steppers.energize itself
       (grbllib.c); Phase 3 adds pulse-timer and input reconfiguration here. */
    (void)settings; (void)changed;
}

static bool driver_setup (settings_t *settings)
{
    IOInitDone = settings->version.id == 23;    /* SETTINGS_VERSION of the pinned core */

    settings_changed(settings, (settings_changed_flags_t){0});

    hal.stepper.go_idle(true);
    hal.coolant.set_state((coolant_state_t){0});

    return IOInitDone;
}

/* --- Entry --- */

bool driver_init (void)
{
    /* Clocks were configured by Reset_Handler/SystemInit; board bring-up: */
    sr_init();
    systick_init();
    systick_hook = systick_hook_fn;

    hal.info = "SAME53 ClearCore";
    hal.driver_version = "260804";
    hal.driver_url = "https://github.com/holla2040/grblhal-clearcore";
    hal.board = "Teknic ClearCore";
    hal.board_url = "https://teknic.com/products/io-motion-controller/";

    hal.driver_setup = driver_setup;
    hal.f_step_timer = F_CPU_HZ;        /* provisional — Phase 3 sets the real stepper timer clock */
    hal.f_mcu = F_CPU_HZ / 1000000UL;
    hal.step_us_min = 1.0f;             /* ClearPath needs >= 715 ns high AND low; Phase 3 tunes */
    hal.rx_buffer_size = RX_BUFFER_SIZE;
    hal.delay_ms = driver_delay_ms;
    hal.get_elapsed_ticks = millis;
    hal.settings_changed = settings_changed;

    hal.stepper.wake_up = stepperWakeUp;
    hal.stepper.go_idle = stepperGoIdle;
    hal.stepper.enable = stepperEnable;
    hal.stepper.cycles_per_tick = stepperCyclesPerTick;
    hal.stepper.pulse_start = stepperPulseStart;

    hal.limits.enable = limitsEnable;
    hal.limits.get_state = limitsGetState;

    hal.coolant.set_state = coolantSetState;
    hal.coolant.get_state = coolantGetState;

    hal.control.get_state = systemGetState;

    hal.irq_enable = __enable_irq;
    hal.irq_disable = __disable_irq;
    hal.set_bits_atomic = bitsSetAtomic;
    hal.clear_bits_atomic = bitsClearAtomic;
    hal.set_value_atomic = valueSetAtomic;

    serialRegisterStreams();

    if(!stream_connect_instance(SERIAL_STREAM, BAUD_RATE))
        while(true);    /* cannot boot without a communication channel */

    hal.nvs.type = NVS_Flash;
    hal.nvs.memcpy_from_flash = nvsRead;
    hal.nvs.memcpy_to_flash = nvsWrite;

    return hal.version == 10;
}
