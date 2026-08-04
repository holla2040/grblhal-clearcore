/*
 * clearcore_map.h — pin map for the Teknic ClearCore as a 4-axis grblHAL
 * board. Axis→connector mapping: X=M-0, Y=M-1, Z=M-2, A=M-3.
 * Sources: PLAN.md Appendix A (from Teknic HardwareMapping.h 1.7.4) +
 * schematic netlist extraction 2026-08-04 (pages 3/4/9/10/12).
 *
 * SIGNAL POLARITY: every motor-connector output (step B, dir A) and the
 * HLFB input pass through inverting 74HC14 stages — the driver bakes that
 * inversion in, so grblHAL's $2/$3 invert masks stay conventional.
 * The B/step pins only reach the connector while the '125 buffer gates
 * (STEP_GATE pins below) are driven LOW.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef CLEARCORE_MAP_H
#define CLEARCORE_MAP_H

#if N_AXIS != 4
#error "ClearCore map is wired for exactly 4 axes (M-0..M-3)"
#endif

#define BOARD_NAME "Teknic ClearCore"

/* Motor step outputs (MtrX_B) — ALL on PORTC */
#define STEP_PORT_GRP   GRP_C
#define X_STEP_PIN      14      /* M-0 B = PC14 */
#define Y_STEP_PIN      15      /* M-1 B = PC15 */
#define Z_STEP_PIN      13      /* M-2 B = PC13 */
#define A_STEP_PIN      10      /* M-3 B = PC10 */

/* Motor direction outputs (MtrX_A) — ALL on PORTA */
#define DIR_PORT_GRP    GRP_A
#define X_DIR_PIN       23      /* M-0 A = PA23 */
#define Y_DIR_PIN       21      /* M-1 A = PA21 */
#define Z_DIR_PIN       22      /* M-2 A = PA22 */
#define A_DIR_PIN       16      /* M-3 A = PA16 */

/* Step-path gate pins: 74AHCT125 output-enables. MUST be driven LOW as
   GPIOs or steps never reach the connectors (4.99k pull-ups block them). */
#define STEP_GATE01_GRP GRP_A   /* Mtr_CLK_01 = PA27 gates M-0/M-1 */
#define STEP_GATE01_PIN 27
#define STEP_GATE23_GRP GRP_B   /* Mtr_CLK_23 = PB23 gates M-2/M-3 */
#define STEP_GATE23_PIN 23

/* HLFB inputs (inverted on the way in), one per motor */
#define X_HLFB_GRP      GRP_A
#define X_HLFB_PIN      20      /* M-0 HLFB = PA20 */
#define Y_HLFB_GRP      GRP_B
#define Y_HLFB_PIN      11      /* M-1 HLFB = PB11 */
#define Z_HLFB_GRP      GRP_C
#define Z_HLFB_PIN      26      /* M-2 HLFB = PC26 */
#define A_HLFB_GRP      GRP_B
#define A_HLFB_PIN      31      /* M-3 HLFB = PB31 */

/* Limit inputs — the four interrupt-capable general-purpose inputs.
   EXTINT line = EIC channel; input conditioning polarity is bench-verified,
   $5 handles inversion. */
#define X_LIMIT_GRP     GRP_C   /* DI-6 = PC16 */
#define X_LIMIT_PIN     16
#define X_LIMIT_EXTINT  0
#define Y_LIMIT_GRP     GRP_C   /* DI-7 = PC17 */
#define Y_LIMIT_PIN     17
#define Y_LIMIT_EXTINT  1
#define Z_LIMIT_GRP     GRP_C   /* DI-8 = PC18 */
#define Z_LIMIT_PIN     18
#define Z_LIMIT_EXTINT  2
#define A_LIMIT_GRP     GRP_B   /* A-9 = PB07 (digital mode via SR bit 31) */
#define A_LIMIT_PIN     7
#define A_LIMIT_EXTINT  7

/* Control inputs (reset / feed hold / cycle start) */
#define RESET_GRP       GRP_B   /* A-10 = PB06 */
#define RESET_PIN       6
#define RESET_EXTINT    6
#define FEED_HOLD_GRP   GRP_B   /* A-11 = PB05 */
#define FEED_HOLD_PIN   5
#define FEED_HOLD_EXTINT 5
#define CYCLE_START_GRP GRP_C   /* A-12 = PC03 */
#define CYCLE_START_PIN 3
#define CYCLE_START_EXTINT 3

/* Probe input — polled (IO-0..5 have no free EXTINT lines) */
#define PROBE_GRP       GRP_C   /* IO-5 input read IN05n = PC19 */
#define PROBE_PIN       19

#endif /* CLEARCORE_MAP_H */
