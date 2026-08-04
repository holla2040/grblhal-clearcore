/**
 * \file
 *
 * \brief Low-level initialization functions called upon chip startup.
 *
 * Copyright (c) 2017 Atmel Corporation,
 *                    a wholly owned subsidiary of Microchip Technology Inc.
 *
 * \asf_license_start
 *
 * \page License
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * \asf_license_stop
 *
 */

/*
 * Adapted from Teknic-Inc/ClearCore-library (tag 1.7.4, commit 4bf5ca6c)
 * libClearCore/src/system_same53.c for grblhal-clearcore.
 * Changes (Phase 1 trim, 2026-08-04): clock macros live in clearcore.h;
 * kept only the core clock path — XOSC1 25 MHz → GCLK5 1 MHz → DPLL1
 * 120 MHz → GCLK0 (cpu), plus DFLL48 shutdown, cache and FPU enable.
 * Removed vs. Teknic: GCLK1 (their step-mask clock), DPLL0/GCLK4 (USB —
 * returns in Phase 5), GCLK6/GCLK7 and all peripheral bus-clock enables
 * and TC/EIC clock routing (each driver claims its own; see RESOURCES.md).
 */

#include "clearcore.h"

/**
 * Initial system clock frequency. The System RC Oscillator (RCSYS) provides
 *  the source for the main clock at chip startup.
 */
// Final CPU speed & DPLL1 frequency
#define __CLEARCORE_CLOCK_HZ    (120000000)             // 120 MHz
#define __SYSTEM_CLOCK          (__CLEARCORE_CLOCK_HZ)
// Oscillator output into XOSC1
#define __CLEARCORE_OSC_HZ      (25000000)              // 25 MHz
// GCLK0 FREQ
#define __CLEARCORE_GCLK0_HZ    __CLEARCORE_CLOCK_HZ
// GCLK4 FREQ (USB)
#define __CLEARCORE_GCLK4_HZ    (48000000)              // 48 MHz
// GCLK5 FREQ (DPLL reference + shift-register SERCOM6 core clock)
#define __CLEARCORE_GCLK5_HZ    (1000000)               // 1 MHz
// DPLL0 FREQ (USB source)
#define __CLEARCORE_DPLL0_HZ    (96000000)              // 96 MHz
// DPLL1 FREQ
#define __CLEARCORE_DPLL1_HZ    (120000000)             // 120 MHz

/*!< System Clock Frequency (Core Clock)*/
uint32_t SystemCoreClock = __SYSTEM_CLOCK;

/**
 * Initialize the system
 *
 * @brief  Setup the microcontroller system.
 *         Initialize the system and update the SystemCoreClock variable.
 */
void SystemInit(void) {
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // Start and setup the various oscillators
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

    // Start the external 10MHz MEMS oscillator
    OSCCTRL->XOSCCTRL[1].reg =
        OSCCTRL_XOSCCTRL_IMULT(4) |
        OSCCTRL_XOSCCTRL_IPTAT(3) |
        OSCCTRL_XOSCCTRL_ENABLE;
    // Wait for clock to run
    while (!OSCCTRL->STATUS.bit.XOSCRDY1) {
        continue;
    }
    // Create 1MHz clock on GCLK5 to act as source for DPLL0/1 and SERCOM6
    GCLK->GENCTRL[5].reg = GCLK_GENCTRL_SRC(GCLK_GENCTRL_SRC_XOSC1_Val) |
                           GCLK_GENCTRL_GENEN |
                           GCLK_GENCTRL_DIV(__CLEARCORE_OSC_HZ /
                                            __CLEARCORE_GCLK5_HZ) |
                           GCLK_GENCTRL_IDC;
    SYNCBUSY_WAIT(GCLK, GCLK_SYNCBUSY_GENCTRL5);

    // Make good 120MHz CPU clock using DPLL1 multiplying GCLK5 up
    SET_CLOCK_SOURCE(OSCCTRL_GCLK_ID_FDPLL1, 5);
    // Set the integer part of the frequency multiplier (loop divider ratio)
    OSCCTRL->Dpll[1].DPLLRATIO.reg =
    OSCCTRL_DPLLRATIO_LDR(__CLEARCORE_DPLL1_HZ / __CLEARCORE_GCLK5_HZ - 1);  
    // Set GCLK as the DPLL clock reference, and set Wake Up Fast
    OSCCTRL->Dpll[1].DPLLCTRLB.reg = OSCCTRL_DPLLCTRLB_REFCLK_GCLK |
                                     OSCCTRL_DPLLCTRLB_WUF;
        
    // Set the DPLL (digital phase-locked loop) to run in standby and sleep mode
    // If ONDEMAND is not set, the signal will be generated constantly
    // Finally, enable the DPLL
    OSCCTRL->Dpll[1].DPLLCTRLA.reg = OSCCTRL_DPLLCTRLA_RUNSTDBY |
                                     OSCCTRL_DPLLCTRLA_ENABLE;

    while (OSCCTRL->STATUS.bit.DPLL1LCKR) {
        continue;
    }
    // Route DPLL1 @ 120MHz to CPU Clock before killing off 48MHz clock we
    // started with.
    GCLK->GENCTRL[0].reg = GCLK_GENCTRL_SRC(GCLK_GENCTRL_SRC_DPLL1_Val) |
                           GCLK_GENCTRL_GENEN |
                           GCLK_GENCTRL_DIV(0);
    SYNCBUSY_WAIT(GCLK, GCLK_SYNCBUSY_GENCTRL0);
    // Clocks running and locked, switch core clock to 120MHz
    MCLK->CPUDIV.reg = MCLK_CPUDIV_DIV_DIV1;

    // DPLL0 @ 96 MHz from GCLK5, then GCLK4 = 48 MHz for USB (Phase 5 claim;
    // this is Teknic's original configuration, restored)
    SET_CLOCK_SOURCE(OSCCTRL_GCLK_ID_FDPLL0, 5);
    OSCCTRL->Dpll[0].DPLLRATIO.reg =
        OSCCTRL_DPLLRATIO_LDR(__CLEARCORE_DPLL0_HZ / __CLEARCORE_GCLK5_HZ - 1);
    OSCCTRL->Dpll[0].DPLLCTRLB.reg = OSCCTRL_DPLLCTRLB_LTIME_DEFAULT |
                                     OSCCTRL_DPLLCTRLB_REFCLK_GCLK |
                                     OSCCTRL_DPLLCTRLB_WUF;
    OSCCTRL->Dpll[0].DPLLCTRLA.reg = OSCCTRL_DPLLCTRLA_ENABLE;

    // Shut down the DFLL48M we booted on — nothing uses it now.
    GCLK->PCHCTRL[OSCCTRL_GCLK_ID_DFLL48].bit.CHEN = 0;
    while (GCLK->PCHCTRL[OSCCTRL_GCLK_ID_DFLL48].bit.CHEN) {
        continue;
    }
    OSCCTRL->DFLLCTRLA.reg = 0;

    // GCLK4 = 48 MHz for USB
    GCLK->GENCTRL[4].reg = GCLK_GENCTRL_GENEN |
                           GCLK_GENCTRL_DIV(__CLEARCORE_DPLL0_HZ /
                                            __CLEARCORE_GCLK4_HZ) |
                           GCLK_GENCTRL_SRC_DPLL0;
    SYNCBUSY_WAIT(GCLK, GCLK_SYNCBUSY_GENCTRL4);

    // Enable the cache controller
    CMCC->CTRL.reg = CMCC_CTRL_CEN;
    // Enable the FPU
    SCB->CPACR = 0xFU << 20;

    while (GCLK->SYNCBUSY.reg) {
        continue;
    }
    return;
}

/**
 * Update SystemCoreClock variable
 *
 * @brief  Updates the SystemCoreClock with current core Clock
 *         retrieved from cpu registers.
 */
void SystemCoreClockUpdate(void) {
    // Not implemented
    return;
}

/* Teknic's GClkFreqUpdate() (XBee baud adjust) removed — unused here. */
