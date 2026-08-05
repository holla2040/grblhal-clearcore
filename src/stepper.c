/*
 * stepper.c — THE CRUX: classic grblHAL two-timer step engine, ours alone.
 *
 * TC4+TC5 chained (COUNT32, MFRQ) = segment timer: MC0 interrupt at prio 0
 * calls hal.stepper.interrupt_callback. TC6 (COUNT16, MFRQ, ONESHOT) =
 * pulse timer: ends the step pulse (and starts it, in the delayed variant).
 * TC6 fires on OVF, not MC0 — in MFRQ CC0 is the period register, so the
 * one-shot's completion at TOP is an overflow, not a compare match.
 * Both clocked from GCLK2 = DPLL1/2 = 60 MHz (hal.f_step_timer).
 * Register idioms cribbed from the grblHAL SAMD21 driver, adapted to the
 * SAME5x TC (dedicated SYNCBUSY register, WAVE register, per-TC IRQ lines).
 *
 * Hardware facts baked in (schematic netlist, PLAN.md Appendix A):
 * - Step/dir/HLFB all pass inverting 74HC14 stages → a LOGICAL 1 (step
 *   asserted at the connector) is a LOW MCU pin. The LUTs below encode it.
 * - The B/step pins reach the connectors only while PA27/PB23 (74AHCT125
 *   output-enables) are driven LOW — done once in stepper_hw_init().
 * - Motor enables are shift-register bits (driver.c stepperEnable).
 * - ClearPath-SD: >= 715 ns step high AND low; board buffers max 500 kHz.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "driver.h"
#include "boards/clearcore_map.h"
#include "stepper.h"

#include "grbl/settings.h"

#define STEP_TIMER_HZ   60000000UL              /* GCLK2 = DPLL1 / 2 */
#define PULSE_TICKS_MIN 48                      /* 800 ns floor > ClearPath 715 ns */

/* Per-axis-combination port masks. Index = 4-bit logical step/dir pattern
   (after user invert). .clr = pins driven LOW (asserted through inverter),
   .set = pins driven HIGH (deasserted). */
typedef struct { uint32_t set; uint32_t clr; } outmask_t;

static outmask_t step_lut[16], dir_lut[16];
static uint32_t pulse_length = 120, pulse_delay = 0;    /* TC6 ticks */
static axes_signals_t next_step_out;
static volatile enum { Pulse_Idle = 0, Pulse_Delaying } pulse_state;

static const uint8_t step_pin[N_AXIS] = { X_STEP_PIN, Y_STEP_PIN, Z_STEP_PIN, A_STEP_PIN };
static const uint8_t dir_pin[N_AXIS]  = { X_DIR_PIN, Y_DIR_PIN, Z_DIR_PIN, A_DIR_PIN };

static inline void set_step_outputs (axes_signals_t step_out)
{
    step_out.value ^= settings.steppers.step_invert.mask;
    PORT->Group[STEP_PORT_GRP].OUTSET.reg = step_lut[step_out.value & 0x0F].set;
    PORT->Group[STEP_PORT_GRP].OUTCLR.reg = step_lut[step_out.value & 0x0F].clr;
}

static inline void set_dir_outputs (axes_signals_t dir_out)
{
    dir_out.value ^= settings.steppers.dir_invert.mask;
    PORT->Group[DIR_PORT_GRP].OUTSET.reg = dir_lut[dir_out.value & 0x0F].set;
    PORT->Group[DIR_PORT_GRP].OUTCLR.reg = dir_lut[dir_out.value & 0x0F].clr;
}

/* Sets up stepper interrupt timeout for the next segment (ISR context). */
static void stepperCyclesPerTick (uint32_t cycles_per_tick)
{
    TC4->COUNT32.CC[0].reg = cycles_per_tick < 60 ? 60 : cycles_per_tick;
    SYNCBUSY_WAIT(&TC4->COUNT32, TC_SYNCBUSY_CC0);
}

static void stepperWakeUp (void)
{
    /* Sensible first timeout; the ISR reloads it before it matters.
       (Motor enables are the core's job via hal.stepper.enable.) */
    stepperCyclesPerTick(STEP_TIMER_HZ / 500);

    TC4->COUNT32.INTFLAG.reg = TC_INTFLAG_MC0;
    TC4->COUNT32.INTENSET.reg = TC_INTENSET_MC0;
    TC4->COUNT32.CTRLBSET.reg = TC_CTRLBSET_CMD_RETRIGGER;
}

static void stepperGoIdle (bool clear_signals)
{
    TC4->COUNT32.CTRLBSET.reg = TC_CTRLBSET_CMD_STOP;
    TC4->COUNT32.INTENCLR.reg = TC_INTENCLR_MC0;
    TC4->COUNT32.INTFLAG.reg = TC_INTFLAG_MC0;

    if(clear_signals) {
        set_step_outputs((axes_signals_t){0});
        set_dir_outputs((axes_signals_t){0});
    }
}

/* Start a step pulse: no-delay variant. */
static void stepperPulseStart (stepper_t *stepper)
{
    if(stepper->dir_changed.bits) {
        stepper->dir_changed.bits = 0;
        set_dir_outputs(stepper->dir_out);
    }

    if(stepper->step_out.bits) {
        set_step_outputs(stepper->step_out);
        TC6->COUNT16.CTRLBSET.reg = TC_CTRLBSET_CMD_RETRIGGER;
    }
}

/* Start a step pulse, delay-after-direction-change variant ($29). */
static void stepperPulseStartDelayed (stepper_t *stepper)
{
    if(stepper->dir_changed.bits) {

        stepper->dir_changed.bits = 0;
        set_dir_outputs(stepper->dir_out);

        if(stepper->step_out.bits) {
            next_step_out = stepper->step_out;      /* defer the pulse */
            pulse_state = Pulse_Delaying;
            TC6->COUNT16.CC[0].reg = (uint16_t)pulse_delay;
            SYNCBUSY_WAIT(&TC6->COUNT16, TC_SYNCBUSY_CC0);
            TC6->COUNT16.CTRLBSET.reg = TC_CTRLBSET_CMD_RETRIGGER;
        }

        return;
    }

    if(stepper->step_out.bits) {
        set_step_outputs(stepper->step_out);
        TC6->COUNT16.CTRLBSET.reg = TC_CTRLBSET_CMD_RETRIGGER;
    }
}

/* Segment timer ISR — prio 0. */
void TC4_Handler (void)
{
    TC4->COUNT32.INTFLAG.reg = TC_INTFLAG_MC0;
    hal.stepper.interrupt_callback();
}

/* Pulse timer ISR — prio 0. Oneshot: ends the pulse (or, when delaying,
   starts it and re-arms for the width). */
void TC6_Handler (void)
{
    TC6->COUNT16.INTFLAG.reg = TC_INTFLAG_OVF | TC_INTFLAG_MC0;

    if(pulse_state == Pulse_Delaying) {
        pulse_state = Pulse_Idle;
        set_step_outputs(next_step_out);
        TC6->COUNT16.CC[0].reg = (uint16_t)pulse_length;
        SYNCBUSY_WAIT(&TC6->COUNT16, TC_SYNCBUSY_CC0);
        TC6->COUNT16.CTRLBSET.reg = TC_CTRLBSET_CMD_RETRIGGER;
    } else
        set_step_outputs((axes_signals_t){0});
}

void stepper_settings_changed (settings_t *settings)
{
    pulse_length = (uint32_t)(settings->steppers.pulse_microseconds * (STEP_TIMER_HZ / 1000000UL));
    if(pulse_length < PULSE_TICKS_MIN)
        pulse_length = PULSE_TICKS_MIN;

    pulse_delay = (uint32_t)(settings->steppers.pulse_delay_microseconds * (STEP_TIMER_HZ / 1000000UL));

    hal.stepper.pulse_start = settings->steppers.pulse_delay_microseconds > 0.0f
                               ? stepperPulseStartDelayed : stepperPulseStart;

    pulse_state = Pulse_Idle;
    TC6->COUNT16.CC[0].reg = (uint16_t)pulse_length;
    SYNCBUSY_WAIT(&TC6->COUNT16, TC_SYNCBUSY_CC0);
}

void stepper_hw_init (void)
{
    uint_fast8_t i, axis;

    /* Build the output LUTs: logical 1 = asserted at connector = pin LOW
       (74HC14 inversion), logical 0 = pin HIGH. */
    for(i = 0; i < 16; i++) {
        for(axis = 0; axis < N_AXIS; axis++) {
            if(i & (1 << axis)) {
                step_lut[i].clr |= 1UL << step_pin[axis];
                dir_lut[i].clr  |= 1UL << dir_pin[axis];
            } else {
                step_lut[i].set |= 1UL << step_pin[axis];
                dir_lut[i].set  |= 1UL << dir_pin[axis];
            }
        }
    }

    /* GPIO: B pins idle deasserted (HIGH), dir deasserted, then unblock the
       step path by driving the '125 gates LOW (they float high otherwise). */
    set_step_outputs((axes_signals_t){0});
    set_dir_outputs((axes_signals_t){0});
    for(axis = 0; axis < N_AXIS; axis++) {
        pin_dir_out(STEP_PORT_GRP, step_pin[axis]);
        pin_dir_out(DIR_PORT_GRP, dir_pin[axis]);
    }

    /* Bench 2026-08-04: HIGH was tried here too (suspected missed inversion
       in the OE path) — no change, step pins dead either way. Back to the
       netlist-derived LOW; the gate is not the fault. */
    pin_write(STEP_GATE01_GRP, STEP_GATE01_PIN, false);
    pin_write(STEP_GATE23_GRP, STEP_GATE23_PIN, false);
    pin_dir_out(STEP_GATE01_GRP, STEP_GATE01_PIN);
    pin_dir_out(STEP_GATE23_GRP, STEP_GATE23_PIN);

    /* GCLK2 (60 MHz, DPLL1/2) is created in SystemInit — it must exist
       before ANY driver init touches TC4/TC5/TC6/TCC4 (SWRST on an
       unclocked timer hangs forever; found the hard way on the bench). */
    CLOCK_ENABLE(APBCMASK, TC4_);
    CLOCK_ENABLE(APBCMASK, TC5_);           /* 32-bit slave of TC4 */
    CLOCK_ENABLE(APBDMASK, TC6_);
    SET_CLOCK_SOURCE(TC4_GCLK_ID, 2);       /* TC4+TC5 share this channel */
    SET_CLOCK_SOURCE(TC6_GCLK_ID, 2);       /* TC6+TC7 share this channel */

    /* TC4+TC5: 32-bit segment timer, MFRQ (CC0 = period), stopped */
    TC4->COUNT32.CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(&TC4->COUNT32, TC_SYNCBUSY_SWRST);
    TC4->COUNT32.CTRLA.reg = TC_CTRLA_MODE_COUNT32 | TC_CTRLA_PRESCALER_DIV1;
    TC4->COUNT32.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
    TC4->COUNT32.CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(&TC4->COUNT32, TC_SYNCBUSY_ENABLE);
    TC4->COUNT32.CTRLBSET.reg = TC_CTRLBSET_CMD_STOP;

    /* TC6: 16-bit one-shot pulse timer, MFRQ, stopped until retriggered */
    TC6->COUNT16.CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(&TC6->COUNT16, TC_SYNCBUSY_SWRST);
    TC6->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV1;
    TC6->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
    TC6->COUNT16.CTRLBSET.reg = TC_CTRLBSET_ONESHOT;
    TC6->COUNT16.CC[0].reg = (uint16_t)pulse_length;
    TC6->COUNT16.CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(&TC6->COUNT16, TC_SYNCBUSY_ENABLE);
    TC6->COUNT16.CTRLBSET.reg = TC_CTRLBSET_CMD_STOP;
    /* Pulse-end interrupt is OVF, NOT MC0: in MFRQ CC0 is the period
       register, so reaching TOP is an overflow, not a compare match. MC0
       never sets here — armed it originally and the pulse never ended
       (bench 2026-08-04: starts=250 irqs=0, INTFLAG.OVF set, MC0 clear). */
    TC6->COUNT16.INTFLAG.reg = TC_INTFLAG_OVF | TC_INTFLAG_MC0;     /* drop flags from the enable-to-stop window */
    TC6->COUNT16.INTENSET.reg = TC_INTENSET_OVF;

    /* Step engine outranks everything (PLAN.md NVIC map: prio 0) */
    NVIC_SetPriority(TC4_IRQn, 0);
    NVIC_SetPriority(TC6_IRQn, 0);
    NVIC_EnableIRQ(TC4_IRQn);
    NVIC_EnableIRQ(TC6_IRQn);
}

void stepper_claim_hal (void)
{
    hal.stepper.wake_up = stepperWakeUp;
    hal.stepper.go_idle = stepperGoIdle;
    hal.stepper.cycles_per_tick = stepperCyclesPerTick;
    hal.stepper.pulse_start = stepperPulseStart;
    /* hal.stepper.enable stays in driver.c — enables are shift-register bits */
}
