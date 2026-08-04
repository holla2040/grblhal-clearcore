/*
 * systick.h — our 1 ms tick: elapsed-ms counter + housekeeping dispatch.
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

void systick_init(void);         /* 1 ms tick at NVIC priority 7 (lowest) */
uint32_t millis(void);
void delay_ms(uint32_t ms);

extern void (*systick_hook)(void);  /* optional per-ms callback, ISR context */
extern volatile uint8_t boot_stage;     /* blink-counted boot telemetry */

#endif /* SYSTICK_H */
