# Upstream PR draft — grblHAL driver for Teknic ClearCore (SAME53)

**DO NOT OPEN THIS PR UNTIL THE BENCH CHECKLIST IN PLAN.md IS GREEN.**
The org should never receive a driver whose exit tests haven't run on
real hardware. When bench is green: fork grblHAL org template or offer
this repo, update the "Verified" section below with actual results, then
open the PR/issue against github.com/grblHAL/core (terjeio has an open
invitation for SAME53/ClearCore in core issue #10).

---

## Title

New driver: ATSAME53 / Teknic ClearCore (bare metal, 4 axes)

## Body

This is a new, zero-vendor-dependency bare-metal driver for the Teknic
ClearCore industrial I/O + motion controller (ATSAME53N19A, Cortex-M4F
@ 120 MHz, 512 KB flash / 192 KB RAM), developed with PlatformIO
(upstream `platform = atmelsam`, no framework).

### Why this board

~$100 industrial controller: 4 servo/stepper connectors with 5 V
buffered step/dir + per-motor feedback (HLFB), 13 optically-conditioned
24 V I/O points, Ethernet 10/100, USB, 2 RS-232/TTL COM ports, microSD.
Teknic's own motion library is architecturally incompatible with GRBL
(bursty 200 µs step windows) — this driver runs the classic grblHAL
two-timer engine on bare metal instead; no vendor library code executes.

### Driver facts

- HAL v10; core pinned as a submodule; hal.f_step_timer = 60 MHz
  (TC4+TC5 32-bit COUNT32 MFRQ segment timer + TC6 one-shot pulse timer,
  both NVIC prio 0)
- NVIC map: 0 = step/pulse, 2 = EIC limits/control, 3 = GMAC,
  4 = SERCOM/USB, 7 = SysTick
- Streams: SERCOM7 UART (board COM-0) + TinyUSB CDC; Ethernet
  (SAME53 GMAC + KSZ8081, lwIP 2.2 NO_SYS) with telnet/websocket/http/mDNS +
  WebUI; SD (SERCOM4 SPI + FatFs) job streaming + G65 macros
- Settings NVS in the last 8 KB flash block (NVMCTRL block erase +
  512 B page writes, CMCC invalidation)
- Board quirks handled: motor enables + COM-port modes + input muxing
  live on a 32-bit shift-register chain (SERCOM6 SPI, refreshed from
  SysTick); step outputs are gated by 74AHCT125 output-enables and
  inverted by 74HC14 buffers (all polarity folded into the driver);
  every 24 V input is industrially active-low (invert defaults set)
- Flash/debug: Atmel-ICE + OpenOCD, sector-erase only (Teknic bootloader
  occupies the first 16 KB; app links at 0x4000)

### Verified (fill from bench before opening)

- [ ] $H homing against real switches
- [ ] 4-axis coordinated G1, commanded vs reported position agree
- [ ] Scoped step pulses evenly spaced at max programmed rate,
      ping-flood during motion shows no disturbance
- [ ] ioSender over USB CDC; telnet + WebUI sessions
- [ ] $F SD job streaming, multi-minute job
- [ ] Settings survive power cycle

### Licensing

Driver files are MIT (author: Craig Hollabaugh); files adapted from the
grblHAL iMXRT1062 driver (enet.c) retain their GPLv3 headers; Teknic-
derived register sequences retain Teknic's MIT notices; Microchip/ARM
headers Apache-2.0; FatFs ChaN license. MIT is GPL-compatible — the org
can take these files into a GPLv3-COPYING repo unchanged.
