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
#include "stepper.h"
#include "spindle_cc.h"
#include "boards/clearcore_map.h"

#if USB_SERIAL_CDC
#include "usb_serial.h"
#endif

#include "grbl/grbllib.h"
#include "grbl/protocol.h"
#include "grbl/settings.h"
#include "grbl/nvs_buffer.h"
#include "grbl/state_machine.h"
#include "grbl/stream.h"
#include "grbl/system.h"

#define LIMIT_EIC_MASK   ((1UL << X_LIMIT_EXTINT) | (1UL << Y_LIMIT_EXTINT) | \
                          (1UL << Z_LIMIT_EXTINT) | (1UL << A_LIMIT_EXTINT))
#define CONTROL_EIC_MASK ((1UL << RESET_EXTINT) | (1UL << FEED_HOLD_EXTINT) | \
                          (1UL << CYCLE_START_EXTINT))

static bool IOInitDone = false;
static bool probe_inverted = false;
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
            while(grbl_delay.ms) {
                if(grbl.on_execute_delay)
                    grbl.on_execute_delay(state_get());
            }
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

/* --- Limits / control / probe: EIC per-line IRQs @ prio 2, EIC hardware
 * debouncer (supersedes the planned TC7 debounce timer), probe polled.
 * Input conditioning polarity is bench-verified; $5/$14/probe-invert
 * settings absorb it. --- */

static void limitsEnable (bool on, axes_signals_t homing_cycle)
{
    (void)homing_cycle;

    EIC->INTFLAG.reg = LIMIT_EIC_MASK;
    if(on)
        EIC->INTENSET.reg = LIMIT_EIC_MASK;
    else
        EIC->INTENCLR.reg = LIMIT_EIC_MASK;
}

static limit_signals_t limitsGetState (void)
{
    limit_signals_t signals = {0};
    uint32_t b = PORT->Group[GRP_B].IN.reg, c = PORT->Group[GRP_C].IN.reg;

    signals.min.x = !!(c & (1UL << X_LIMIT_PIN));
    signals.min.y = !!(c & (1UL << Y_LIMIT_PIN));
    signals.min.z = !!(c & (1UL << Z_LIMIT_PIN));
    signals.min.a = !!(b & (1UL << A_LIMIT_PIN));

    signals.min.value ^= settings.limits.invert.mask;

    return signals;
}

static control_signals_t systemGetState (void)
{
    control_signals_t signals = {0};
    uint32_t b = PORT->Group[GRP_B].IN.reg, c = PORT->Group[GRP_C].IN.reg;

    signals.reset = !!(b & (1UL << RESET_PIN));
    signals.feed_hold = !!(b & (1UL << FEED_HOLD_PIN));
    signals.cycle_start = !!(c & (1UL << CYCLE_START_PIN));

    signals.value ^= settings.control_invert.value;

    return signals;
}

static void probeConfigure (bool is_probe_away, bool probing)
{
    (void)probing;

    probe_inverted = is_probe_away ? !settings.probe.invert_probe_pin
                                   : settings.probe.invert_probe_pin;
}

static probe_state_t probeGetState (void)
{
    probe_state_t state = { .connected = On };

    state.triggered = (!!(PORT->Group[PROBE_GRP].IN.reg & (1UL << PROBE_PIN))) ^ probe_inverted;

    return state;
}

/* EIC line ISRs — prio 2 */

static void limit_isr (uint32_t eic_line)
{
    EIC->INTFLAG.reg = 1UL << eic_line;
    hal.limits.interrupt_callback(limitsGetState());
}

static void control_isr (uint32_t eic_line)
{
    EIC->INTFLAG.reg = 1UL << eic_line;
    hal.control.interrupt_callback(systemGetState());
}

void EIC_EXTINT_0_Handler (void) { limit_isr(X_LIMIT_EXTINT); }
void EIC_EXTINT_1_Handler (void) { limit_isr(Y_LIMIT_EXTINT); }
void EIC_EXTINT_2_Handler (void) { limit_isr(Z_LIMIT_EXTINT); }
void EIC_EXTINT_7_Handler (void) { limit_isr(A_LIMIT_EXTINT); }
void EIC_EXTINT_6_Handler (void) { control_isr(RESET_EXTINT); }
void EIC_EXTINT_5_Handler (void) { control_isr(FEED_HOLD_EXTINT); }
void EIC_EXTINT_3_Handler (void) { control_isr(CYCLE_START_EXTINT); }

/* Configure an input pin; eic = also route it to the EIC (pmux A) */
static void pin_input_init (uint8_t grp, uint8_t pin, bool eic)
{
    PORT->Group[grp].DIRCLR.reg = 1UL << pin;
    PORT->Group[grp].PINCFG[pin].reg = PORT_PINCFG_INEN;
    if(eic)
        pin_pmux(grp, pin, 0);      /* function A = EIC (keeps INEN set) */
}

static void inputs_init (void)
{
    static const uint8_t eic_irqs[] = { X_LIMIT_EXTINT, Y_LIMIT_EXTINT, Z_LIMIT_EXTINT,
                                        A_LIMIT_EXTINT, RESET_EXTINT, FEED_HOLD_EXTINT,
                                        CYCLE_START_EXTINT };
    uint_fast8_t i;

    CLOCK_ENABLE(APBAMASK, EIC_);
    SET_CLOCK_SOURCE(EIC_GCLK_ID, 5);       /* GCLK5 = 1 MHz */

    pin_input_init(X_LIMIT_GRP, X_LIMIT_PIN, true);
    pin_input_init(Y_LIMIT_GRP, Y_LIMIT_PIN, true);
    pin_input_init(Z_LIMIT_GRP, Z_LIMIT_PIN, true);
    pin_input_init(A_LIMIT_GRP, A_LIMIT_PIN, true);
    pin_input_init(RESET_GRP, RESET_PIN, true);
    pin_input_init(FEED_HOLD_GRP, FEED_HOLD_PIN, true);
    pin_input_init(CYCLE_START_GRP, CYCLE_START_PIN, true);

    pin_input_init(PROBE_GRP, PROBE_PIN, false);        /* polled */

    pin_input_init(X_HLFB_GRP, X_HLFB_PIN, false);      /* polled (Phase 3: digital) */
    pin_input_init(Y_HLFB_GRP, Y_HLFB_PIN, false);
    pin_input_init(Z_HLFB_GRP, Z_HLFB_PIN, false);
    pin_input_init(A_HLFB_GRP, A_HLFB_PIN, false);

    EIC->CTRLA.bit.ENABLE = 0;
    SYNCBUSY_WAIT(EIC, EIC_SYNCBUSY_ENABLE);

    /* Both-edge sense on all used lines (0,1,2,3,5,6,7 — all in CONFIG[0]);
       the ISRs read actual pin state, so edge direction doesn't matter. */
    EIC->CONFIG[0].reg = EIC_CONFIG_SENSE0_BOTH | EIC_CONFIG_SENSE1_BOTH |
                         EIC_CONFIG_SENSE2_BOTH | EIC_CONFIG_SENSE3_BOTH |
                         EIC_CONFIG_SENSE5_BOTH | EIC_CONFIG_SENSE6_BOTH |
                         EIC_CONFIG_SENSE7_BOTH;

    /* Hardware debounce: 1 MHz / 2^(7+1) = 3.9 kHz sampling, 7-sample
       majority ≈ 1.8 ms — replaces the provisional TC7 debounce timer. */
    EIC->DEBOUNCEN.reg = LIMIT_EIC_MASK | CONTROL_EIC_MASK;
    EIC->DPRESCALER.reg = EIC_DPRESCALER_PRESCALER0(7) | EIC_DPRESCALER_STATES0;

    EIC->CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(EIC, EIC_SYNCBUSY_ENABLE);

    /* Control signals are always armed; limits armed via limitsEnable() */
    EIC->INTFLAG.reg = CONTROL_EIC_MASK | LIMIT_EIC_MASK;
    EIC->INTENSET.reg = CONTROL_EIC_MASK;

    for(i = 0; i < sizeof(eic_irqs); i++) {
        NVIC_SetPriority((IRQn_Type)(EIC_EXTINT_0_IRQn + eic_irqs[i]), 2);
        NVIC_EnableIRQ((IRQn_Type)(EIC_EXTINT_0_IRQn + eic_irqs[i]));
    }
}

/* --- HLFB monitor: poll servo feedback, alarm on fault while enabled.
 * HLFB asserted at the connector reads as MCU pin LOW (74HC14 inversion).
 * Compile-time gated until HLFB polarity is bench-verified with a
 * configured ClearPath (MSP "Servo On" HLFB mode). --- */

#if USB_SERIAL_CDC

static on_execute_realtime_ptr on_execute_realtime_usb_prev;
static on_execute_realtime_ptr on_execute_delay_usb_prev;

static void usb_rt_poll (sys_state_t state)
{
    usb_poll();
    on_execute_realtime_usb_prev(state);
}

static void usb_delay_poll (sys_state_t state)
{
    usb_poll();
    on_execute_delay_usb_prev(state);
}

#endif /* USB_SERIAL_CDC */

#if HLFB_MONITOR_ENABLE

static on_execute_realtime_ptr on_execute_realtime_prev;

static void hlfb_poll (sys_state_t state)
{
    static uint32_t last_ms = 0;

    uint32_t ms = millis();

    if(ms - last_ms >= 10) {
        last_ms = ms;

        uint32_t sr = sr_get();
        bool fault =
            ((sr & SR_EN_OUT_0) && (PORT->Group[X_HLFB_GRP].IN.reg & (1UL << X_HLFB_PIN))) ||
            ((sr & SR_EN_OUT_1) && (PORT->Group[Y_HLFB_GRP].IN.reg & (1UL << Y_HLFB_PIN))) ||
            ((sr & SR_EN_OUT_2) && (PORT->Group[Z_HLFB_GRP].IN.reg & (1UL << Z_HLFB_PIN))) ||
            ((sr & SR_EN_OUT_3) && (PORT->Group[A_HLFB_GRP].IN.reg & (1UL << A_HLFB_PIN)));

        if(fault && state != STATE_ALARM)
            system_raise_alarm(Alarm_MotorFault);
    }

    on_execute_realtime_prev(state);
}

#endif /* HLFB_MONITOR_ENABLE */

/* --- Coolant: flood = IO-3, mist = IO-0, connector LEDs mirror state --- */

static void coolantSetState (coolant_state_t mode)
{
    coolant_state = mode;
    mode.value ^= settings.coolant.invert.mask;

    pin_write(COOLANT_FLOOD_GRP, COOLANT_FLOOD_PIN, mode.flood ^ IO_OUT_INVERT);
    pin_write(COOLANT_MIST_GRP, COOLANT_MIST_PIN, mode.mist ^ IO_OUT_INVERT);

    if(coolant_state.flood)
        sr_set(SR_LED_IO_3);
    else
        sr_clear(SR_LED_IO_3);
    if(coolant_state.mist)
        sr_set(SR_LED_IO_0);
    else
        sr_clear(SR_LED_IO_0);
}

static coolant_state_t coolantGetState (void)
{
    return coolant_state;
}

static void outputs_init (void)
{
    pin_write(COOLANT_FLOOD_GRP, COOLANT_FLOOD_PIN, IO_OUT_INVERT ? true : false);
    pin_write(COOLANT_MIST_GRP, COOLANT_MIST_PIN, IO_OUT_INVERT ? true : false);
    pin_dir_out(COOLANT_FLOOD_GRP, COOLANT_FLOOD_PIN);
    pin_dir_out(COOLANT_MIST_GRP, COOLANT_MIST_PIN);
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
    (void)changed;

    if(IOInitDone) {
        stepper_settings_changed(settings);     /* pulse width/delay → TC6 */
        probeConfigure(false, false);
    }
}

static bool driver_setup (settings_t *settings)
{
    stepper_hw_init();      /* GCLK2, TC4/TC5 + TC6, step/dir GPIO, '125 gates low */
    inputs_init();          /* EIC limits/control + probe/HLFB inputs */
    outputs_init();         /* coolant pins (spindle pins init in spindle_cc_register) */

#if USB_SERIAL_CDC
    on_execute_realtime_usb_prev = grbl.on_execute_realtime;
    grbl.on_execute_realtime = usb_rt_poll;
    on_execute_delay_usb_prev = grbl.on_execute_delay;
    grbl.on_execute_delay = usb_delay_poll;
#endif

#if HLFB_MONITOR_ENABLE
    on_execute_realtime_prev = grbl.on_execute_realtime;
    grbl.on_execute_realtime = hlfb_poll;
#endif

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
    hal.f_step_timer = 60000000UL;      /* GCLK2 = DPLL1/2 feeding TC4/TC5 */
    hal.f_mcu = F_CPU_HZ / 1000000UL;
    hal.step_us_min = 0.8f;             /* ClearPath needs >= 715 ns high AND low */
    hal.rx_buffer_size = RX_BUFFER_SIZE;
    hal.delay_ms = driver_delay_ms;
    hal.get_elapsed_ticks = millis;
    hal.settings_changed = settings_changed;

    stepper_claim_hal();                /* wake_up/go_idle/cycles_per_tick/pulse_start */
    hal.stepper.enable = stepperEnable; /* enables are shift-register bits */

    hal.limits.enable = limitsEnable;
    hal.limits.get_state = limitsGetState;

    hal.coolant.set_state = coolantSetState;
    hal.coolant.get_state = coolantGetState;

    hal.control.get_state = systemGetState;

    hal.probe.get_state = probeGetState;
    hal.probe.configure = probeConfigure;

    spindle_cc_register();

    hal.signals_cap.reset = On;
    hal.signals_cap.feed_hold = On;
    hal.signals_cap.cycle_start = On;
    hal.limits_cap.min.mask = AXES_BITMASK;
    hal.coolant_cap.flood = On;
    hal.coolant_cap.mist = On;
    hal.driver_cap.probe = On;
    hal.driver_cap.step_pulse_delay = On;

    hal.irq_enable = __enable_irq;
    hal.irq_disable = __disable_irq;
    hal.set_bits_atomic = bitsSetAtomic;
    hal.clear_bits_atomic = bitsClearAtomic;
    hal.set_value_atomic = valueSetAtomic;

    serialRegisterStreams();

#if USB_SERIAL_CDC
    if(!stream_connect(usb_serialInit()))
        while(true);    /* cannot boot without a communication channel */
#else
    if(!stream_connect_instance(SERIAL_STREAM, BAUD_RATE))
        while(true);    /* cannot boot without a communication channel */
#endif

    hal.nvs.type = NVS_Flash;
    hal.nvs.memcpy_from_flash = nvsRead;
    hal.nvs.memcpy_to_flash = nvsWrite;

    return hal.version == 10;
}
