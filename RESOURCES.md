# RESOURCES.md — peripheral claim table

**Every** timer, SERCOM, GCLK, DMA channel, EVSYS channel, EIC line, and IRQ
this firmware uses gets a row here **when the claim is made**. The 2021
failure included unmanaged timer collisions — this table is the immune system.
No code may touch a peripheral that is not recorded here.

| Resource | Claimed by | Purpose | NVIC prio | Phase |
|---|---|---|---|---|
| XOSC1 (25 MHz MEMS osc) | system_same53.c | root oscillator | — | 1 |
| DPLL1 (120 MHz, ref GCLK5) | system_same53.c | CPU clock PLL | — | 1 |
| GCLK0 = 120 MHz (DPLL1) | system_same53.c | CPU + SERCOM7 core | — | 1 |
| GCLK5 = 1 MHz (XOSC1/25) | system_same53.c | DPLL0/1 reference + SERCOM6 core | — | 1 |
| CMCC (flash cache) | system_same53.c | enabled | — | 1 |
| SysTick (120 MHz/1000) | systick.c | 1 ms tick, elapsed-ms, SR refresh dispatch | 7 | 1 |
| SERCOM6 (SPI master) | shiftreg.c | 32-bit shift-register chain, SCK 500 kHz | none (serviced from SysTick) | 1 |
| pins PC05/PC06/PC07 (pmux C) | shiftreg.c | SR SCK / DATA_RET / DATA | — | 1 |
| pins PB01, PB02 (GPIO out) | shiftreg.c | SR_ENn (active low), SR_LOAD strobe | — | 1 |
| SERCOM7 (USART) + IRQs SERCOM7_0 (DRE), SERCOM7_2 (RXC) | serial.c | COM-0 grblHAL stream 115200-8N1 | 4 | 1→2 |
| pins PB20, PB21 (pmux D) | serial.c | COM-0 RX (PAD1) / TX (PAD0) | — | 1 |
| NVMCTRL + flash block 0x7E000–0x7FFFF (8 KB, outside linker FLASH) | driver.c | grblHAL settings NVS | — | 2 |
| GCLK2 = 60 MHz (DPLL1/2) | stepper.c | TC4/TC5 + TC6 clock (`hal.f_step_timer`) | — | 3 |
| TC4+TC5 (COUNT32, MFRQ) + TC4_IRQn | stepper.c | segment/stepper timer | **0** | 3 |
| TC6 (COUNT16, MFRQ, ONESHOT) + TC6_IRQn | stepper.c | step-pulse one-shot (width + optional delay) | **0** | 3 |
| pins PC10/PC13/PC14/PC15 (GPIO out) | stepper.c | step B lines M-3/M-2/M-0/M-1 (inverted at connector) | — | 3 |
| pins PA16/PA21/PA22/PA23 (GPIO out) | stepper.c | dir A lines M-3/M-1/M-2/M-0 (inverted at connector) | — | 3 |
| pins PA27, PB23 (GPIO out, driven LOW) | stepper.c | '125 step-path gates — HIGH blocks all steps | — | 3 |
| EIC + lines 0,1,2,7 (limits X/Y/Z/A) | driver.c | limit IRQs, hw debounce ≈1.8 ms | 2 | 3 |
| EIC lines 6,5,3 (reset/feed-hold/cycle-start) | driver.c | control IRQs, hw debounce | 2 | 3 |
| pins PC16/PC17/PC18/PB07 (pmux A, INEN) | driver.c | limits DI-6/7/8 + A-9 | — | 3 |
| pins PB06/PB05/PC03 (pmux A, INEN) | driver.c | control A-10/A-11/A-12 | — | 3 |
| pin PC19 (INEN) | driver.c | probe (IO-5 input read, polled) | — | 3 |
| pins PA20/PB11/PC26/PB31 (INEN) | driver.c | HLFB M-0..M-3 (polled; monitor compile-gated) | — | 3 |

## Planned claims (from PLAN.md §3 — provisional until the row moves up)

| Resource | Purpose |
|---|---|
| free TCC | spindle PWM (Phase 4) |
| ADC1 | A-9..A-12 analog reads (Phase 4) |
| DPLL0 (96 MHz) + GCLK4 (48 MHz) | USB (Phase 5 — Teknic's config was removed from system_same53.c, re-add there) |
| GMAC + PHY_INT EXTINT12 | Ethernet (Phase 6), prio 3 |
| SERCOM4 + GCLK route | microSD SPI (Phase 7; Teknic used GCLK7 @ 10 MHz) |

## Free (verified unclaimed)

TC0–TC3, **TC7** (was the planned debounce timer — not needed: the SAME5x
EIC has a hardware debouncer, used instead; dated note in PLAN.md Phase 3),
TCC0–TCC4, all DMA channels, all EVSYS channels, ADC0/ADC1, DAC, SERCOM0
(COM-1), SERCOM2 (XBee header), GCLK1/3/6/7, EIC lines 8/9/13/14 (lines
4/10/11/15 = HLFB EXTINTs reserved for optional interrupt fault detection;
line 12 = Ethernet PHY_INT, Phase 6), RTC, WDT, PDEC, QSPI.
