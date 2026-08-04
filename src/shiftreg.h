/*
 * shiftreg.h — ClearCore 32-bit output shift-register chain driver.
 * Motion-critical: motor enables, COM-port modes, analog-input muxing and
 * all LEDs live on this chain (bit map in clearcore.h / PLAN.md Appendix A).
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef SHIFTREG_H
#define SHIFTREG_H

#include <stdint.h>

void sr_init(void);              /* SERCOM6 SPI + strobe pins; outputs enabled on return */
void sr_set(uint32_t mask);      /* set logical bits (atomic, any context) */
void sr_clear(uint32_t mask);    /* clear logical bits (atomic, any context) */
void sr_toggle(uint32_t mask);   /* toggle logical bits (atomic, any context) */
uint32_t sr_get(void);           /* current logical state */
void sr_refresh(void);           /* latch previous word, shift out current — SysTick context */

#endif /* SHIFTREG_H */
