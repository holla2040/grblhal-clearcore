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
| SERCOM7 (USART) + IRQs SERCOM7_0 (DRE), SERCOM7_2 (RXC) | uart.c | COM-0 console 115200-8N1 | 4 | 1 |
| pins PB20, PB21 (pmux D) | uart.c | COM-0 RX (PAD1) / TX (PAD0) | — | 1 |

## Planned claims (from PLAN.md §3 — provisional until the row moves up)

| Resource | Purpose |
|---|---|
| TC4+TC5 (chained 32-bit) | stepper/segment timer, prio 0 (Phase 3) |
| TC6 | step-pulse one-shot, prio 0 (Phase 3) |
| pins PA27, PB23 (GPIO out, LOW) | step-path '125 buffer enables — MUST be driven low or steps are blocked (Phase 3) |
| TC7 | debounce (Phase 3) |
| EIC + lines: DI-6/7/8+A-9 limits, A-10..12 control | prio 2 (Phase 3) |
| free TCC | spindle PWM (Phase 4) |
| DPLL0 (96 MHz) + GCLK4 (48 MHz) | USB (Phase 5 — Teknic's config was removed from system_same53.c, re-add there) |
| GMAC + PHY_INT EXTINT12 | Ethernet (Phase 6), prio 3 |
| SERCOM4 + GCLK route | microSD SPI (Phase 7; Teknic used GCLK7 @ 10 MHz) |
| NVMCTRL (last flash pages) | grblHAL settings NVS (Phase 2) |

## Free (verified unclaimed)

TC0–TC3, TCC0–TCC4, all DMA channels, all EVSYS channels, ADC0/ADC1, DAC,
SERCOM0 (COM-1), SERCOM2 (XBee header), GCLK1/2/3/6/7, RTC, WDT, PDEC, QSPI.
GCLK6→TC4/TC6 and GCLK1 routings from Teknic's clock tree were removed in the
Phase 1 trim — TC4/TC6 clock sources are unrouted until Phase 3 claims them.
