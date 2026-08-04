# RESOURCES.md — peripheral claim table

**Every** timer, SERCOM, GCLK, DMA channel, EVSYS channel, EIC line, and IRQ
this firmware uses gets a row here **when the claim is made**. The 2021
failure included unmanaged timer collisions — this table is the immune system.
No code may touch a peripheral that is not recorded here.

| Resource | Claimed by | Purpose | NVIC prio | Phase |
|---|---|---|---|---|
| *(none claimed yet)* | | | | |

## Planned claims (from PLAN.md §3 — provisional until the row moves up)

| Resource | Purpose |
|---|---|
| TC4+TC5 (chained 32-bit) | stepper/segment timer, prio 0 |
| TC6 | step-pulse one-shot, prio 0 |
| TC7 | debounce |
| free TCC | spindle PWM |
| SysTick | 1 ms housekeeping tick, prio 7 |

## Inherited state to re-audit in Phase 1

The vendored `src/system_same53.c` (Teknic tag 1.7.4, content unchanged) still
configures Teknic's clock tree and bus clocks: XOSC1 25 MHz, DPLL0 96 MHz,
DPLL1 120 MHz, GCLK0 (120 MHz cpu), GCLK1 (500 kHz), GCLK4 (48 MHz USB),
GCLK5 (1 MHz PLL ref), GCLK6 (2.048 MHz, routed to EIC/TC0/TC4/TC6),
GCLK7 (10 MHz SPI), and enables bus clocks for SERCOM0/2/4/7, TC0/3/4/5/6,
EIC, EVSYS, GMAC, ADC1, CMCC cache, FPU. **None of these are claims** — the
Phase 1 clock-tree task trims this to what we use and records real claims
above (note: GCLK6→TC4/TC6 routing conflicts with our stepper-timer plan and
must be redone).
