/*
 * spindle.c — PWM spindle for grblhal-clearcore, registered via
 * spindle_register() (current core API, idiom from the SAM3X8E driver).
 *
 * Outputs: enable = IO-1 (24 V, inverted group), direction = IO-2 (same),
 * PWM = IO-4 through the DRV8844 half-bridge: PB16 = EN3 (active high,
 * gates the whole output; low = high-Z = safe off), PB14 = IN3 driven by
 * TCC4/WO0. IN3 LOW turns the load on, so WO0 is hardware-inverted via
 * DRVCTRL.INVEN0 and the PWM math stays conventional. TCC4 is 16-bit;
 * a prescaler is auto-picked so low $33 frequencies still fit.
 * Falls back to on/off mode when PWM precompute fails ($30=$31 etc.).
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "driver.h"
#include "boards/clearcore_map.h"
#include "shiftreg.h"
#include "spindle_cc.h"

#include "grbl/settings.h"

#define SPINDLE_PWM_CLK 60000000UL      /* GCLK2 */

static spindle_pwm_t spindle_pwm;
static uint32_t pwm_prescaler_bits = TCC_CTRLA_PRESCALER_DIV1_Val;

static inline void io_out (uint8_t grp, uint8_t pin, bool on)
{
    pin_write(grp, pin, on ^ IO_OUT_INVERT);
}

static inline bool io_out_state (uint8_t grp, uint8_t pin)
{
    return (!!(PORT->Group[grp].OUT.reg & (1UL << pin))) ^ IO_OUT_INVERT;
}

static inline void spindle_on_off (bool on)
{
    io_out(SPINDLE_ENABLE_GRP, SPINDLE_ENABLE_PIN, on);
    /* DRV8844 EN3 is active HIGH, no inverter — NOT part of IO_OUT_INVERT */
    pin_write(SPINDLE_PWMEN_GRP, SPINDLE_PWMEN_PIN, on);

    if(on)
        sr_set(SR_LED_IO_1 | SR_LED_IO_4);
    else
        sr_clear(SR_LED_IO_1 | SR_LED_IO_4);
}

static inline void spindle_dir (bool ccw)
{
    io_out(SPINDLE_DIR_GRP, SPINDLE_DIR_PIN, ccw ^ settings.pwm_spindle.invert.ccw);
}

/* --- basic on/off mode (PWM precompute failed or $ config disables it) --- */

static void spindleSetState (spindle_ptrs_t *spindle, spindle_state_t state, float rpm)
{
    (void)spindle; (void)rpm;

    if(!state.on)
        spindle_on_off(false);
    else {
        spindle_dir(state.ccw);
        spindle_on_off(true);
    }
}

/* --- variable (PWM) mode --- */

static void spindleSetSpeed (spindle_ptrs_t *spindle, uint_fast16_t pwm_value)
{
    if(pwm_value == spindle->context.pwm->off_value) {
        if(spindle->context.pwm->flags.rpm_controlled)
            spindle_on_off(false);
        TCC4->CCBUF[0].reg = spindle->context.pwm->flags.always_on
                              ? spindle->context.pwm->off_value : 0;
    } else {
        if(!spindle->context.pwm->flags.enable_out && spindle->context.pwm->flags.rpm_controlled)
            spindle_on_off(true);
        TCC4->CCBUF[0].reg = pwm_value;
    }
}

static uint_fast16_t spindleGetPWM (spindle_ptrs_t *spindle, float rpm)
{
    return spindle->context.pwm->compute_value(spindle->context.pwm, rpm, false);
}

static void spindleSetStateVariable (spindle_ptrs_t *spindle, spindle_state_t state, float rpm)
{
    if(!(spindle->context.pwm->flags.cloned ? state.ccw : state.on)) {
        spindleSetSpeed(spindle, spindle->context.pwm->off_value);
        spindle_on_off(false);
    } else {
        if(!spindle->context.pwm->flags.cloned)
            spindle_dir(state.ccw);
        if(rpm == 0.0f && spindle->context.pwm->flags.rpm_controlled)
            spindle_on_off(false);
        else {
            spindle_on_off(true);
            spindleSetSpeed(spindle, spindle->context.pwm->compute_value(spindle->context.pwm, rpm, false));
        }
    }
}

static spindle_state_t spindleGetState (spindle_ptrs_t *spindle)
{
    spindle_state_t state = {0};

    state.on = io_out_state(SPINDLE_ENABLE_GRP, SPINDLE_ENABLE_PIN);
    state.ccw = io_out_state(SPINDLE_DIR_GRP, SPINDLE_DIR_PIN) ^ settings.pwm_spindle.invert.ccw;

    if(spindle && spindle->param)
        state.on = state.on || spindle->param->rpm != 0.0f;

    return state;
}

static bool spindleConfig (spindle_ptrs_t *spindle)
{
    /* TCC4 is 16-bit: pick the smallest prescaler that fits the period */
    static const uint16_t divs[8] = { 1, 2, 4, 8, 16, 64, 256, 1024 };

    if(spindle == NULL)
        return false;

    uint_fast8_t p = 0;
    if(settings.pwm_spindle.pwm_freq > 0.0f) {
        while(p < 7 && (float)SPINDLE_PWM_CLK / divs[p] / settings.pwm_spindle.pwm_freq > 65535.0f)
            p++;
    }
    pwm_prescaler_bits = p;

    if(spindle_precompute_pwm_values(spindle, &spindle_pwm, &settings.pwm_spindle,
                                      SPINDLE_PWM_CLK / divs[p])) {

        TCC4->CTRLA.bit.ENABLE = 0;
        SYNCBUSY_WAIT(TCC4, TCC_SYNCBUSY_ENABLE);

        TCC4->CTRLA.reg = TCC_CTRLA_PRESCALER(pwm_prescaler_bits);
        /* No hardware inversion: PB14 (DRV8844 IN3) -> OUT4 -> IO-4 is
           non-inverting pin-to-connector. The DRV8844 "IN low = load on"
           note describes load WIRING (load between IO-4 and Vsupply), not
           an inversion in the signal path, so baking INVEN0 in here was
           wrong. Polarity is the user's via $16 bit 2, which the core
           already applies in software (spindle_control.c invert_pwm());
           keying DRVCTRL off the same setting double-inverted and made
           $16 a no-op (bench 2026-08-04). */
        TCC4->DRVCTRL.reg = 0;
        TCC4->WAVE.reg = TCC_WAVE_WAVEGEN_NPWM;
        TCC4->PER.reg = spindle_pwm.period;
        TCC4->CC[0].reg = spindle_pwm.off_value;

        TCC4->CTRLA.bit.ENABLE = 1;
        SYNCBUSY_WAIT(TCC4, TCC_SYNCBUSY_ENABLE);

        spindle->set_state = spindleSetStateVariable;
    } else {
        if(spindle->context.pwm->flags.enable_out)
            spindle->set_state(spindle, (spindle_state_t){0}, 0.0f);
        spindle->set_state = spindleSetState;
    }

    spindle_update_caps(spindle, spindle->cap.variable ? &spindle_pwm : NULL);

    return true;
}

void spindle_cc_register (void)
{
    static const spindle_ptrs_t spindle = {
        .type = SpindleType_PWM,
        .ref_id = SPINDLE_PWM0,
        .config = spindleConfig,
        .set_state = spindleSetStateVariable,
        .get_state = spindleGetState,
        .get_pwm = spindleGetPWM,
        .update_pwm = spindleSetSpeed,
        .cap = {
            .gpio_controlled = On,
            .variable = On,
            .laser = On,
            .direction = On,
            /* spindleConfig() already honours invert.pwm, but without this
               cap the core rejects the $16 PWM bit (Status_SettingDisabled)
               and forces it Off — the invert path was unreachable. Whether
               the pin should track speed (VFD signal) or its complement
               (DRV8844 load, IN low = on) depends on wiring, so it belongs
               to the user, not a compile-time guess. */
            .pwm_invert = On
        }
    };

    /* 24 V outputs off (inverted group idles HIGH), DRV8844 EN3 low = high-Z */
    io_out(SPINDLE_ENABLE_GRP, SPINDLE_ENABLE_PIN, false);
    io_out(SPINDLE_DIR_GRP, SPINDLE_DIR_PIN, false);
    pin_write(SPINDLE_PWMEN_GRP, SPINDLE_PWMEN_PIN, false);
    pin_dir_out(SPINDLE_ENABLE_GRP, SPINDLE_ENABLE_PIN);
    pin_dir_out(SPINDLE_DIR_GRP, SPINDLE_DIR_PIN);
    pin_dir_out(SPINDLE_PWMEN_GRP, SPINDLE_PWMEN_PIN);

    /* TCC4 on GCLK2; full timer config happens in spindleConfig() */
    CLOCK_ENABLE(APBDMASK, TCC4_);
    SET_CLOCK_SOURCE(TCC4_GCLK_ID, 2);
    TCC4->CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(TCC4, TCC_SYNCBUSY_SWRST);

    pin_pmux(SPINDLE_PWM_GRP, SPINDLE_PWM_PIN, SPINDLE_PWM_MUX);

    spindle_register(&spindle, "ClearCore PWM");
}
