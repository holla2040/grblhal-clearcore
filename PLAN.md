# PLAN.md — grblHAL on Teknic ClearCore (Bare-Metal, 4-Axis)

> **TO THE AGENT PICKING THIS UP:** This is the **working document** for this project.
> You have none of the conversation that produced it — everything you need is in this file.
> Rules of engagement:
> 1. Read this entire document before writing any code.
> 2. Work the phases in order. Each phase has checkboxes; **check them off (`- [x]`) as you complete tasks** and keep this file updated — it is the shared progress tracker for every agent that touches this project.
> 3. When you finish a work session, append a dated entry to the **Session Log** at the bottom: what you did, what you learned, what's blocked.
> 4. If you discover something that invalidates part of this plan, do not silently deviate — update the plan text, mark the change with a dated note, and explain in the Session Log.
> 5. **Commit and push throughout development — this is standing authorization from the owner for this repo.** Commit at every logical milestone (a completed checkbox, a newly working state, before any risky refactor) with a message that names the plan item; push at minimum at the end of every work session and every completed phase, so GitHub always reflects real progress and any agent/machine can pick up from `origin`. Stage files individually by path (never `git add -A` or `git add .`).
> 6. **THE PRIME DIRECTIVE (§Architecture) IS NOT NEGOTIABLE: no Teknic library code ever executes in this firmware.** Copying their MIT-licensed source as adapted fragments is encouraged; linking/initializing/running their library is forbidden.
> 7. **Work as an orchestrator.** Delegate substantial self-contained tasks to subagents, and choose the model to fit the task: cheap/fast models (haiku, sonnet) for mechanical sweeps, bulk research, and boilerplate; top-tier models (fable, opus) for architecture decisions, ISR/driver code, and adversarial review of anything safety- or correctness-critical. Always verify a subagent's output yourself before checking a box — the checkbox is the orchestrator's signature, not the subagent's.
> 8. Hardware-gated exit tests (flashing, scope measurements, motor moves) may be deferred: stack up desk work, and list pending bench steps for the owner in the Session Log rather than blocking on them. Much can be verified motor-free: logic analyzer/scope on step pins, HLFB inputs jumpered, settings and streams exercised over UART/USB, grblHAL core logic compiled and unit-poked host-side.

---

**License (owner decision 2026-08-04):** This project is **MIT** — see `LICENSE`. All new files carry the project MIT license. Files adapted from third parties keep their original notices: Teknic ClearCore-library (MIT), Microchip/Atmel startup/system files (Apache-2.0), Arduino LLC linker script (LGPL-2.1). The grblHAL core submodule is GPLv3, so the *linked firmware binary as a whole* is distributed under GPLv3 terms; MIT is GPL-compatible, so this costs nothing and keeps Phase 8 upstreaming clean.

---

## 1. Context — why this project exists and why it failed once

The owner (Craig, holla2040) has 3× Teknic ClearCore controllers (~$105 ea) and several ClearPath-SD closed-loop servo motors (~$1,500 of hardware, purchased 2021). Goal: a modern, standalone, 4-axis G-code motion controller running on the ClearCore, developed with **PlatformIO on Linux**, driving the ClearPath-SD motors via step/direction.

**The 2021 attempt** (archived at `/oldhome/holla/clearcore_projects/src/grbl_clearcore/`, GitHub holla2040/clearcore_projects — a separate, frozen repo; do not develop there) got a grblHAL driver as far as: working serial console, SD-card settings persistence, coolant stubs. It died at step generation: **the Teknic library's 5 kHz periodic interrupt (200 µs) was observed disrupting the pulse stream**, Teknic provided no support, and the project was shelved.

**2026 re-diagnosis (verified against library source v1.7.4):**
- `TCC0_0_Handler` fires every 200 µs at NVIC priority 3 and runs a heavyweight refresh (ADC, all connectors, USB, CCIO, encoder, shift register). Anything generating pulses at priority ≥3 gets stomped every 200 µs. This is what was observed in 2021.
- The library's own step generation is **bursty by design**: each 200 µs window, the ISR hands a step COUNT to TCC0/TCC1 hardware, which emits that many pulses then goes silent until the next window. TCC0/TCC1 are therefore owned by the library, and the 2021 code unknowingly assigned both to other jobs (`driver.h:55`: TCC1 as debounce timer; `clearcore.h:55`: TCC0 as spindle PWM) — a guaranteed collision.
- The library contains ~15 `__disable_irq()` global critical sections (MotorDriver, StepGenerator, UsbManager, SysManager, EncoderInput, CcioBoardManager). Each is brief (sub-µs), but each blanks ALL interrupts including a priority-0 step ISR.
- **Owner's decision: eliminate the entire risk class. The Teknic library never runs. Period.**

**Firmware choice: grblHAL** (github.com/grblHAL/core) — decided after a comprehensive 2026 survey vs µCNC, FluidNC, Klipper, Remora/LinuxCNC, RepRapFirmware, Smoothieware v2, g2core. grblHAL is the only candidate that is simultaneously: standalone (no host PC), ≥4 axes (max 8), CNC-deep dialect (canned cycles G73/G81–G89, G76 threading, G33 spindle-sync, G65/O-word macros, NGC expressions/parameters, G43 TLO, M6/ATC, M62–M68 digital/analog I/O, Modbus VFD plugin), maintained daily (2026), portable via a ~4-file driver, and comfortably inside 512 KB flash. Runner-up RRF needs ~1 MB; FluidNC is ESP32/WiFi-centric with no Ethernet backend; Klipper/Remora demote the ClearCore to a dumb peripheral behind a mandatory host.

**Other locked decisions:**
- **No RTOS.** Bare-metal. Determinism comes from NVIC priority (step ISR at 0–1 outranks everything). All proven ARM grblHAL drivers are bare-metal; lwIP runs NO_SYS. FreeRTOS would add work and a known SysTick conflict for zero motion benefit.
- **Step engine: classic grblHAL two-timer model only.** Evenly spaced pulses from our own timer ISR. The Teknic burst-window architecture is explicitly rejected (it is the 200 µs hiccup source).
- **Committed scope:** core 4-axis motion, homing/limits/probing, spindle/coolant, settings in internal flash, UART + USB streaming, Ethernet (Telnet/WebSocket/WebUI), SD job streaming, upstream the driver to the grblHAL org.

## 2. Hardware facts

| Item | Value |
|---|---|
| MCU | ATSAME53N19A, Cortex-M4F @ 120 MHz, **512 KB flash / 192 KB RAM**, 100-TQFP |
| Bootloader | Teknic UF2/SAM-BA bootloader in first **16 KB**; application linked at **0x4000**. **NEVER chip-erase** — sector-erase only, or you destroy the bootloader (recovery procedure must be documented in README before first flash). |
| Motors | 4 connectors M-0..M-3, step/dir/enable through 5V buffers, one HLFB input each. **500 kHz is the ClearCore board's max step rate**; the ClearPath-SD motor itself accepts up to 700 kHz with a **minimum pulse width of 715 ns high AND low** (constrains grblHAL's `$0` step-pulse setting). Input resolution configurable 200–6400 steps/rev via Teknic MSP software (**Windows-only** — needs a Windows box/VM with USB pass-through) — **configure motors to 800 steps/rev initially** (1600 optional later) so required step rates stay ≤~100 kHz. |
| HLFB | Per-motor feedback line; modes: static servo-on, PWM duty (0–100%), bipolar PWM (torque). Phase 3 uses simple digital read (OK/fault). |
| I/O | IO-0..IO-5 (out-capable, IO-0 analog-out, IO-4/5 H-bridge-capable), DI-6..DI-8 (digital in), A-9..A-12 (analog/digital in). All 24 V buffered through connector circuits — never raw MCU pins. |
| Board quirk | A 32-bit **shift register** (normally refreshed by the Teknic tick) carries the **motor enables**, COM-port mode muxing, analog-input mode switching, and all LEDs — full bit map in Appendix A. We must drive it ourselves (Phase 1; motion-critical). Protocol: `libClearCore/src/ShiftRegister.cpp` (241 lines). |
| Comms | USB (target port), 2× COM ports (SERCOM UART), Ethernet 10/100 (SAME53 GMAC + PHY), microSD (SERCOM SPI), encoder input option. |
| Debug | **Atmel-ICE on hand** (SWD). Known-good OpenOCD configs in `src/grbl_clearcore/clearcore.cfg` / `clearcore-debug.cfg` (cmsis-dap, `target/atsame5x.cfg`, program at 0x4000). |

## 3. Architecture — THE PRIME DIRECTIVE

**No interrupt fires and nothing periodic executes unless we wrote it.**

- The Teknic library (`Teknic-Inc/ClearCore-library`, MIT) is a **reference manual and parts bin**, never a runtime dependency. Copy leaf code, adapt it, own it, call it only from our contexts. No SysManager, no FastUpdate, no their-SysTick, no their-USB, no their-step-generation.
- NVIC priority map (ours, 3 priority bits): **0 = step timer + pulse timer ISRs. 1 = reserved. 2 = EIC limits/control/probe. 3 = GMAC. 4+ = UART/USB/SPI/DMA. 7 = SysTick housekeeping (our 1 ms tick: delay/elapsed counters + shift-register refresh).**
- All peripherals are ours to allocate. Record EVERY claim (timer, SERCOM, GCLK, DMA channel, EVSYS channel, IRQ) in `RESOURCES.md` as it is made. The 2021 failure included unmanaged timer collisions — this table is the immune system.
- Provisional timer plan: TC4+TC5 chained 32-bit = stepper/segment timer; TC6 = step-pulse one-shot (width + optional delay); TC7 = debounce; any free TCC = spindle PWM. (All verifiable free — nothing else is running.)

## 4. Resources (local paths verified 2026-08-04)

| Resource | Path / URL |
|---|---|
| This repo (active work) | `/home/holla/grblhal-clearcore` (remote: github.com/holla2040/grblhal-clearcore) |
| 2021 archive repo (frozen — salvage only) | `/oldhome/holla/clearcore_projects` (remote: github.com/holla2040/clearcore_projects) |
| 2021 driver to salvage | `src/grbl_clearcore/` — **salvage verbatim:** `clearcore.cfg`, `clearcore-debug.cfg`, the `Makefile` flash/reset recipes (the `program … verify reset 0x4000` OpenOCD invocation lives in the Makefile, NOT in the .cfg files), atomic helpers (driver.c:157–177), `hal.xls` (checklist). **Reference only:** driver.c wiring shape, clearcore.h pin ideas. **Do not reuse:** timer defines (collisions), serial.cpp (fake `serialRxFree`!), file.cpp (SD-settings), stepper/spindle/limits (empty or UB — `limitsGetState()` has no return statement). |
| Teknic library clone (v1.7.4) | Re-clone to a stable location: `git clone https://github.com/Teknic-Inc/ClearCore-library` (a session-scratchpad clone existed at planning time; scratchpads are ephemeral — re-clone. Appendix A was extracted from commit `4bf5ca6c6c3f02bef1b8cbed4a31c4d5130cb09a`, tag 1.7.4; use that or newer and note here if the pin data changes: ______ ) |
| Copy candidates in that clone | `ProjectTemplate/Device_Startup/{startup_same53.c, flash_with_bootloader.ld}` · `libClearCore/src/system_same53.c` (245-line clock init to 120 MHz) · `libClearCore/inc/HardwareMapping.h` (MCU-pin↔connector map — key facts pre-extracted into Appendix A below, from commit `4bf5ca6c` / tag 1.7.4) · `libClearCore/src/ShiftRegister.cpp` + `inc/ShiftRegister.h` (bit map in Appendix A) · `libClearCore/src/EthernetManager.cpp` (470 lines) + `LwIP/` tree · `NvmManager.cpp`, `SdCardDriver.cpp`, `UsbManager.cpp` as references |
| Schematic (authoritative cross-check) | `/oldhome/holla/reference/t/teknic/core/docs/ClearCore-Electrical-Schematic.pdf` (also `sch` symlink) |
| Hardware manual / datasheet / ClearPath manual | `/oldhome/holla/reference/t/teknic/core/docs/clearcore_user_manual.pdf`, `SAM_D5x_E5x_Family_Data_Sheet_DS60001507G.pdf`, `/oldhome/holla/reference/t/teknic/path/docs/clearpath_user_manual.pdf` |
| PlatformIO board JSON + SVD | `/oldhome/holla/reference/t/teknic/core/platformio/teknic_clearcore.json`, `ATSAME53N19A.svd` (vendor both into the new project) |
| grblHAL | core: github.com/grblHAL/core — **pin the submodule to a recorded master commit SHA** (do NOT use the release tags: only two exist and the newest, 20250518, is >14 months stale while master moves daily and the reference drivers track master; a tag pin would bake in the exact API-drift problem §6.4 warns about). Advance the pin deliberately, once per phase. Driver skeleton: github.com/grblHAL/Templates `arm-driver/`. Register-idiom cribs: github.com/grblHAL/SAM3X8E (active), github.com/grblHAL/SAMD21 (EOL but same SERCOM/TC/TCC/EIC/GCLK lineage). Plugins: networking, sdcard. HAL reference: svn.io-engineering.com/grblHAL/html/hal_8h.html. Porting guidance from terjeio to this exact project: github.com/grblHAL/core issues #10 (2021). |
| USB stack | TinyUSB (MIT; SAME5x port mature) |
| Senders for testing | ioSender (grblHAL-native, Windows), gSender (cross-platform) |
| Settings sanity baseline | `/oldhome/holla/grbl.conf` (a real machine's `$$` dump) |
| PlatformIO note | Use **upstream** `platform = atmelsam` with **no framework** (bare-metal) + local `boards/teknic_clearcore.json`. The SAME5x toolchain/openocd already ship (adafruit_feather_m4_can is SAME51). PR platformio/platform-atmelsam#215 (ClearCore board, Arduino framework) exists but is NOT a dependency of this plan. |

## 5. Phases and progress

### Phase 0 — Repo + frameworkless PlatformIO build (est. 6–10 h)
- [x] Create project repo `grblhal-clearcore` — this repo, `/home/holla/grblhal-clearcore` (git initialized 2026-08-04; remote: `git@github.com:holla2040/grblhal-clearcore.git`)
- [x] Add `LICENSE` — **MIT** (owner decision 2026-08-04, supersedes the planned GPLv3: repo's own files are MIT; the linked firmware binary as a whole is still GPLv3 because the grblHAL core submodule is GPLv3 — MIT is GPL-compatible, Phase 8 upstreaming unaffected; license statement added to PLAN.md preamble). Every file adapted from ClearCore-library KEEPS Teknic's MIT copyright header plus a one-line "adapted from …, changes: …" note (MIT license condition); Microchip/Atmel files keep their Apache-2.0 headers; new files carry the project MIT header.
- [x] Add `.gitignore`: `.pio/`, build artifacts (`*.o`, `*.elf`, `*.bin` — EXCEPT the bootloader dump below), editor droppings
- [x] `platformio.ini`: upstream `platform = atmelsam`, **no `framework` line** (bare build), `board = teknic_clearcore`. Frameworkless mechanics — this is the wiring that makes it actually build: `board_build.ldscript = ld/flash_with_bootloader.ld` in platformio.ini (the vendored board JSON's `build.arduino.ldscript` key is Arduino-framework-only and does nothing in a bare build); put `startup_same53.c`/`system_same53.c` under `src/` (PlatformIO auto-compiles `src/` in bare builds); `build_flags = -I<DFP include dirs> -D__SAME53N19A__ -D__SAME53__` (+ `build_src_filter = +<*> -<grbl/>` so the core submodule at `src/grbl` stays out of the build until Phase 2)
- [x] Vendor `boards/teknic_clearcore.json` + `debug/ATSAME53N19A.svd` + salvaged OpenOCD cfgs + the 2021 Makefile's flash/reset recipes (board JSON cleaned for bare-metal: dropped Arduino framework/core/variant keys and the Arduino-style `__ATSAME53N19A__`/`__SAMD53__` defines — DFP uses `__SAME53N19A__`; cfgs at repo root; Makefile adds dump-bootloader/restore-bootloader targets)
- [x] Vendor CMSIS + SAME53 DFP headers from **Microchip packs** — done 2026-08-04: `vendor/dfp` = **SAME53_DFP 2.0.11**, `vendor/cmsis` = ARM CMSIS 6.3.0 core headers (details in vendor/README.md). **⚠ Discovered 2026-08-04: all SAME53_DFP 3.x releases ship Microchip's NEW Harmony-style headers (`GCLK_REGS->…`, `_UINT32_`, no `DeviceVectors`, no `.reg`/`.bit` structs) — incompatible with the Teknic-adapted files and with every grblHAL reference driver idiom. 2.0.11 is the newest ASF/CMSIS-Atmel-style version; do not "upgrade" the DFP past 2.x.** Consequences applied: sources include `<sam.h>` (ships in 2.0.11 alongside `same53.h`), and the startup vector table was mechanically renamed to 2.0.11's `DeviceVectors` member names (`EIC_EXTINT_n`/`TCCn_MCn`/`DAC_EMPTY_n`-style — the Teknic file targeted older 1.x names).
- [x] Copy + adapt `startup_same53.c`, `system_same53.c` (into `src/`), `flash_with_bootloader.ld` (into `ld/`; ORIGIN 0x4000). Adaptations (2026-08-04): startup got a self-contained `Reset_Handler` (data/bss init + **`SCB->VTOR = &exception_table`** — Teknic's library never sets VTOR anywhere — + SystemInit + libc init + main); Teknic's original Reset_Handler lives in SysManager.cpp and boots their library, so it was rewritten, not copied. system_same53.c: SysUtils.h macros inlined, clock content otherwise untouched (Phase 1 trims it — see RESOURCES.md "inherited state" section).
- [x] Add grblHAL core as git submodule at `src/grbl`, **pinned to a recorded master commit SHA** (record here: `306bf68d520eeadadafdb94a8ad9eb6be3c8d96b`, master 2026-08-04)
- [x] Create `RESOURCES.md` (peripheral/IRQ/GCLK/DMA/EVSYS claim table — starts empty; also lists provisional planned claims and the Teknic clock-tree state inherited via system_same53.c that Phase 1 must re-audit)
- [ ] **BEFORE ANY FLASH OPERATION — dump the bootloader** (the only irreversible failure mode in this project; Teknic does not publish the bootloader binary): `openocd -f clearcore.cfg -c "init; dump_image bootloader-16k.bin 0x0 0x4000; shutdown"` → verify non-blank (not all-0xFF), commit it to the repo. Recovery procedure = `program bootloader-16k.bin verify 0x0`. Document both in README, along with the sector-erase-only rule; optionally check the BOOTPROT fuse as a second protection layer.
- [ ] **EXIT TEST:** minimal `main()` builds, links at 0x4000, flashes via Atmel-ICE (sector erase), debugger halts in `main()`, bootloader still enumerates after double-tap reset — **desk half verified 2026-08-04**: builds clean (2936 B flash / 1140 B RAM), `exception_table` at 0x4000, entry = `Reset_Handler`, firmware.bin first words = SP 0x20030000 / reset 0x42b9. **Bench half pending (owner)**: dump-bootloader first, then `make flash`, debugger halt in `main()` (watch `counter` increment), double-tap-reset enumeration.

### Phase 1 — Bare-metal board bring-up (est. 12–24 h)
- [ ] Clock tree to 120 MHz (adapt system_same53.c); verify by scoping a GPIO toggle or GCLK output — **desk half done 2026-08-04**: trimmed to XOSC1→GCLK5(1 MHz)→DPLL1→GCLK0(120 MHz) + DFLL shutdown + CMCC/FPU; Teknic's GCLK1/4/6/7, DPLL0 and all peripheral bus-clock enables removed (drivers claim their own — RESOURCES.md updated). Scope verify = bench.
- [x] Our SysTick @ 1 ms, priority 7: elapsed-ms counter (`hal.get_elapsed_ticks` source) + housekeeping dispatch (src/systick.c: millis/delay_ms + SR refresh dispatch; 1 Hz underglow blink in for the exit test)
- [x] Shift-register driver (src/shiftreg.c, done 2026-08-04): SERCOM6 SPI master @500 kHz, 32-bit LSB-first, latch-previous strobe, refresh from SysTick, atomic set/clear/toggle, Teknic's inversion mask replicated (`SR_INVERT 0xFFF300F3`). Power-on defaults: motors disabled, COM ports TTL UART, A-9..12 digital, underglow on. **Schematic cross-check passed (p12, netlist-extracted, 2026-08-04):** 4× SN74AHCT595 @5 V; all five MCU pins, chain order (U9→U11→U20→U13) and all 32 bit assignments confirmed against Appendix A; `SR_ENn` has a 4.99 kΩ pull-up → outputs high-Z at power-on until PB01 driven low (sr_init sequence handles this); latch = rising edge on STCP ('595 datasheet fact); `SR_DATA_RET` returns a 5 V level into PC06 through only 2 kΩ (stock Teknic design — clamp current <1 mA, acceptable).
- [x] UART console on COM-0 SERCOM7 (src/uart.c, done 2026-08-04): interrupt-driven @ prio 4, TX/RX ring buffers, 115200-8N1, real rx_free accounting. TTL mode is on by hardware default (polarity pulled down, UART/SPI mux pulled up) AND held by sr_init. **RJ-45 pinout extracted to README** (p9, netlist-verified): 1=RTS out, 2/6=+5V, 3=CTS in, 4/7=GND, 5=TX out, 8=RX in; the sheet's PoE note ("GND on 4 and 8") is a typo — netlist says 4 and 7. Bench: echo test pending.
- [ ] `pio debug` with Atmel-ICE: breakpoints + SVD register view confirmed working
- [ ] **EXIT TEST:** LED blinks from SysTick housekeeping; UART echoes; 1 ms tick verified on scope

### Phase 2 — grblHAL skeleton boot (est. 12–20 h)
- [x] `driver.c`/`driver.h` from arm-driver template, wired against CURRENT hal API (done 2026-08-04, HAL v10 — the Templates skeleton targets v8 and current SAM3X8E was the real crib; API drift hit once: `settings.steppers.deenergize` is now `energize` and the core applies it itself at startup). Real in Phase 2: stream, NVS, delay/elapsed-ticks, **motor enables via shift-register bits**; safe no-ops until Phases 3/4: stepper motion, limits, control inputs, coolant outputs. `enumerate_pins` for `$pins` deferred to Phase 3 (needs the board pin map). New file `syscalls.c`: newlib stubs + real `_sbrk` (core uses malloc; 8 KB stack reserve — PlatformIO's bare link ignores `--specs=` flags).
- [x] `my_machine.h` at driver root (N_AXIS 4, plugins off) — force-included into EVERY compilation unit via `-include src/my_machine.h` + `-DOVERRIDE_MY_MACHINE`: the core never includes my_machine.h itself (org builds inject config as `-D` flags via the Web Builder; force-include is the bare-metal equivalent that keeps core and driver TUs agreeing on N_AXIS)
- [x] UART as first grblHAL stream (serial.c supersedes Phase 1 uart.c: stream buffer types from core stream.h, `enqueue_realtime_command` interception in RXC ISR, real `rx_free`, claim/register via `stream_register_streams` + `stream_connect_instance`)
- [x] NVS: our NVMCTRL driver on the last 8 KB flash block at 0x7E000 (block-erase + 512 B page writes + CMCC cache invalidation after write; block carved OUT of the linker FLASH region so code can never land there; NVS_SIZE 2048 = exactly 4 pages)
- [x] Port atomic helpers from 2021 driver.c (driver.c:157-177 verbatim — the standard disable-irq trio)
- [ ] **EXIT TEST:** grblHAL banner over UART; `$$` lists settings; a changed setting survives power cycle; `?` status responds; 0x18 soft-reset works

### Phase 3 — Stepper: our two-timer engine (est. 24–40 h) — THE CRUX
- [ ] `RESOURCES.md` first: record TC4+TC5 (32-bit stepper timer, prio 0), TC6 (pulse one-shot, prio 0), TC7 (debounce), EIC lines, GCLKs
- [ ] Step/dir GPIO for M-0..M-3 — exact pins in **Appendix A** (B line = step, A line = dir); enable = shift-register bits 8–11 with ClearPath enable-handshake timing per ClearPath manual
- [x] **Schematic verification before first pulse** — resolved 2026-08-04 by netlist extraction from the schematic (pages 3/4/10), ahead of schedule. The B pin does NOT reach the connector directly: `MtrX_B` feeds a 74AHCT125 buffer whose **output-enable is `Mtr_CLK_01` (PA27, motors 0/1) / `Mtr_CLK_23` (PB23, motors 2/3)**; the buffer output has a 2 kΩ pull-up to 5 V, then a 74HC14 **inverter**, then 130 Ω to connector pin 2. Both CLK pins have 4.99 kΩ pull-ups to 3V3 → they float HIGH → buffer high-Z → connector held LOW → **steps are blocked at power-on**. Consequences for our driver: (1) claim PA27+PB23 as GPIO outputs driven LOW at stepper init — then each `MtrX_B` is a direct per-axis step source; (2) the connector step signal is INVERTED (B idle high ⇒ connector idle low; a step pulse = drive B low for the pulse width); (3) the A/dir path (74HC14) and HLFB input path (74HC14) are ALSO inverted — set driver-default step/dir invert masks and HLFB read inversion accordingly. Teknic's own sheet note confirms the masking design. Bench scope check happens anyway in the bring-up ladder below.
- [ ] `stepper.c`: wake_up, go_idle, enable, cycles_per_tick, pulse_start (+pulse_delay variant), TC4 ISR → `hal.stepper.interrupt_callback` (crib register idioms from SAM3X8E/SAMD21 drivers)
- [ ] HLFB digital read per motor; poll from `on_execute_realtime` → alarm/E-stop on servo fault; expose as aux inputs
- [ ] Limits + control (reset/feed-hold/cycle-start) on EIC per-line IRQs @ prio 2, TC7 debounce — **only the Appendix A "interrupt-capable inputs" can do this** (suggested: limits on DI-6/7/8 + A-9, control on A-10..A-12); probe on a polled input (grblHAL polls probe during G38 — no EXTINT needed)
- [ ] `boards/clearcore_map.h`: N_AXIS 4, all pins as our own connector-indexed defines
- [ ] Configure ClearPath motors to 800 steps/rev (Teknic MSP software — **Windows-only**, needs a Windows box/VM with USB pass-through; owner bench step) and record settings here
- [ ] Bring-up ladder: single-axis jog → **scope step pins: pulse width + spacing under UART load (there is no foreign code — any jitter is ours)** → single-axis homing → 4-axis coordinated moves
- [ ] **EXIT TEST:** `$H` homes an axis against a real switch; 4-axis G1 completes with all HLFBs asserted and commanded-vs-reported position agreeing; scoped pulses evenly spaced at max programmed rate

### Phase 4 — I/O, spindle, coolant, probe (est. 12–20 h)
- [ ] `ioports.c` via `ioports_add_digital()` / `ioports_add_analog()` (NOT `ioports_add()` — deprecated in current core, ioports.h:312): IO-0..IO-5 out, DI-6..DI-8 in, A-9..A-12 analog in (our bare-metal ADC) → M62–M68 + `$pins`
- [ ] Spindle: `spindle_register()` — options per Appendix A: PWM on IO-4/IO-5 H-bridge PWM pads (TCC3/TCC4), or **analog spindle via IO-0's DAC circuit (Aout00 = PA02/DAC0 + SR bit 20)** for VFDs wanting 0-10V/4-20 mA; enable/dir on IO-1/IO-2
- [ ] Coolant flood/mist on two outputs; probe input with invert support
- [ ] **EXIT TEST:** `M3 S12000` correct duty on scope; `M62 P0` toggles the mapped output; `G38.2` captures and retracts

### Phase 5 — USB CDC via TinyUSB (est. 10–16 h)
- [ ] TinyUSB device (SAME5x), CDC-ACM as second grblHAL stream (UART stays)
- [ ] 1200-baud-touch → bootloader reset in CDC callback (keeps `bossac` flashing usable; double-tap reset is hardware fallback)
- [ ] **EXIT TEST:** ioSender connects + streams over USB; UART console still live; bootloader flash path verified

### Phase 6 — Ethernet (est. 24–40 h)
- [ ] GMAC driver + PHY init (adapt Teknic EthernetManager/netif glue — running under OUR ISR at prio 3)
- [ ] lwIP NO_SYS raw mode; `sys_check_timeouts()` + RX pump from `on_execute_realtime` (proven iMXRT/STM32H7 grblHAL pattern)
- [ ] grblHAL networking plugin: Telnet first, then WebSocket, then HTTP + WebUI (WebUI assets need Phase 7 FS — may land after SD)
- [ ] **EXIT TEST:** job streams over Telnet with USB/UART still responsive; ping flood during a 4-axis move shows no step disturbance on the scope

### Phase 7 — SD job streaming (est. 8–12 h)
- [ ] SERCOM SPI driver (SdCardDriver as reference) + FatFs + grblHAL sdcard plugin
- [ ] **EXIT TEST:** `$F` lists files; `$F=/job.nc` runs a multi-minute 4-axis job standalone; WebUI upload+run if HTTP landed

### Phase 8 — Upstream (est. 4–8 h)
- [ ] Restructure to grblHAL org driver conventions (`my_machine.h`, board map file)
- [ ] PR the SAME53/ClearCore driver to the grblHAL org (a zero-vendor-dependency bare-metal driver is the easiest shape to accept)
- [ ] **EXIT TEST:** PR opened; local/fork builds green against the pinned core and current master; maintainer feedback addressed (org CI/merge timing is the maintainer's, not ours)

## 6. Risks
1. **Board bring-up unknowns** (shift register, connector buffers, COM levels) → schematic + Teknic source as executable documentation; hard-gated in Phase 1.
2. **TinyUSB/bootloader touch-reset quirks** → UART console from Phase 1 keeps everything unblocked; Atmel-ICE is the primary flash path throughout.
3. **GMAC/lwIP integration is ours** → adapt MIT glue; Telnet-first isolates transport from HTTP.
4. **grblHAL core API drift** → pin core per phase; crib only from active drivers; Phase 8 upstreaming ends the exposure.
5. **Bootloader damage** → 0x4000 sector-erase-only flashing everywhere; recovery documented before first flash.

## 7. Effort guide
Phases 0–7 ≈ 110–180 h. A usable 4-axis machine (UART streaming, homing, probing, spindle) exists at end of Phase 4 (~65–115 h).

---

## Appendix A — Hardware pin quick-reference

Extracted 2026-08-04 from `Teknic-Inc/ClearCore-library` commit `4bf5ca6c6c3f02bef1b8cbed4a31c4d5130cb09a` (tag 1.7.4): `HardwareMapping.h`, `ShiftRegister.h`, `SysManager.cpp:204-211`. Treat as authoritative starting point; **cross-check against the schematic PDF before trusting in anger**. Pin notation: SAME53 PORT pin (e.g. PA23 = PORTA pin 23).

### Motor connectors (per SysManager.cpp constructor wiring)

| Conn | A line (= DIR in step/dir mode) | B line (= STEP; Teknic muxes to TCC — we re-mux to GPIO) | HLFB input | Enable |
|---|---|---|---|---|
| M-0 | PA23 | PC14 (TCC0.4 pad) | PA20 — **EXTINT4 free** | shift-reg bit 11 (`SR_EN_OUT_0`) |
| M-1 | PA21 | PC15 (TCC0.5 pad) | PB11 — **EXTINT11 free** | shift-reg bit 10 (`SR_EN_OUT_1`) |
| M-2 | PA22 | PC13 (TCC0.3 pad) | PC26 — **EXTINT10 free** | shift-reg bit 9 (`SR_EN_OUT_2`) |
| M-3 | PA16 | PC10 (TCC0.0 pad) | PB31 — **EXTINT15 free** | shift-reg bit 8 (`SR_EN_OUT_3`) |

**Consequences:** (1) `hal.stepper.enable` drives shift-register bits, not GPIO — the Phase 1 shift-register driver is **motion-critical**. (2) HLFB lines all have free EXTINT lines → interrupt-capable fault detection; **HLFB is inverted through a 74HC14 on its way in** (schematic p3, 2026-08-04). (3) **[RESOLVED 2026-08-04 — see the checked Phase 3 schematic-verification task]** `Mtr_CLK_01` (PA27) / `Mtr_CLK_23` (PB23) are output-enables of a 74AHCT125 in the B/step path: drive both LOW as GPIOs and each `MtrX_B` becomes a direct per-axis step source, inverted at the connector (as is the A/dir path). Left floating (4.99 kΩ pull-ups high) steps are blocked. (4) Teknic routes HLFB PWM capture via per-motor EVSYS channels — reference for the optional HLFB-duty feature later.

### Interrupt-capable inputs (EXTINT line free) — the complete list, budget accordingly

| Pin | MCU | EXTINT | Notes |
|---|---|---|---|
| DI-6 | PC16 | 0 | also quadrature A pad |
| DI-7 | PC17 | 1 | also quadrature B pad |
| DI-8 | PC18 | 2 | also quadrature index pad |
| A-9 | PB07 | 7 | ADC1 ch9; analog-vs-digital mode = SR bits 28–31 (A-9 = bit 31) |
| A-10 | PB06 | 6 | ADC1 ch8 |
| A-11 | PB05 | 5 | ADC1 ch7 |
| A-12 | PC03 | 3 | ADC1 ch5 |
| (HLFB ×4) | PA20/PB11/PC26/PB31 | 4/11/10/15 | reserved for motor fault |
| PHY_INT | PC28 | 12 | Ethernet PHY (Phase 6) |

**IO-0..IO-5 input reads have NO free EXTINT lines — polling only.** Suggested budget: 4 axis limits on DI-6/7/8 + A-9 (EXTINT); reset/feed-hold/cycle-start on A-10..A-12 (EXTINT); probe on a polled input (grblHAL polls probe state during G38 cycles — no interrupt needed).

### I/O connector output/input pins

| Conn | Output drive | Input read | Extras |
|---|---|---|---|
| IO-0 | OUT00 PA00 | IN00n PA05 | analog out: Aout00 PA02 (DAC0) + SR bit 20 mode |
| IO-1 | OUT01 PA01 | IN01n PB17 | |
| IO-2 | OUT02 PA06 | IN02n PA03 | |
| IO-3 | OUT03 PA07 | IN03n PC21 | |
| IO-4 | OUT04 PB16 | IN04n PC27 | H-bridge: PWM04A PB14 / PWM04B PB15 / polarity bits |
| IO-5 | OUT05 PB03 | IN05n PC19 | H-bridge: PWM05A PB12 / PWM05B PB13 |

### Communications

| Function | SERCOM | Pins |
|---|---|---|
| COM-0 (RJ-45, 5V TTL) | SERCOM7 | TX PB21, RX PB20, RTS PB18, CTS PB19; mode = SR bits 13/17 |
| COM-1 (RJ-45, 5V TTL) | SERCOM0 | TX PA08, RX PA09, RTS PA10, CTS PA11; mode = SR bits 12/16 |
| USB device | — | DM PA24, DP PA25 |
| microSD (SPI) | SERCOM4 | MOSI PB08, SCK PB09, MISO PB10, SS PA04 |
| Shift register | (bit-bang or SERCOM6) | SR_CLK PC05, SR_DATA PC07, SR_DATA_RET PC06, SR_LOAD PB02, SR_ENn PB01 |
| Ethernet RMII + MDIO | GMAC | TXEN PA17, TXD0 PA18, TXD1 PA19, TXCLK PA14, RXD0 PA13, RXD1 PA12, RXDV PC20, RXER PA15, MDC PC11, MDIO PC12, PHY_INT PC28 |
| SWD debug | — | SWCLK PA30, SWDIO PA31, SWO PB30 |
| XBee header (unused) | SERCOM2 | PB24/PB25/PC24/PC25 |

### Shift-register bit map (32-bit chain, ShiftRegister.h:134-166)

| Bits | Function |
|---|---|
| 0–1 | `A_CTRL_3`, `A_CTRL_2` (M-3/M-2 A-line mode control) |
| 2–7 | LEDs IO-5..IO-0 |
| 8–11 | **motor enables: `EN_OUT_3`(8), `EN_OUT_2`(9), `EN_OUT_1`(10), `EN_OUT_0`(11)** |
| 12–13 | COM-1 / COM-0 TTL-vs-RS232 polarity (`UART_TTL_x`) |
| 14–15 | underglow LED, USB LED |
| 16–17 | COM-1 / COM-0 UART-vs-SPI mux (`UART_SPI_SEL_x`) |
| 18–19 | COM-0 / COM-1 LEDs |
| 20 | IO-0 analog-out mode (`CFG00_AOUT`) |
| 21–23 | DI-6/7/8 LEDs |
| 24–27 | A-12..A-9 LEDs |
| 28–31 | A-12..A-9 analog-vs-digital input mode (`ANAIN_DIGITAL_x`) |

### Timer-allocation note
Our claimed timers (TC4+TC5 stepper, TC6 pulse, TC7 debounce) are used as **internal interrupt timers with no waveform outputs** — the fact that various pins carry TC4/TC5/TC6/TC7 pad functions (e.g. OUT04=TC6.0 pad, Mtr1_HLFB=TC5.1 pad) is **irrelevant**: pad routing only matters if the timer's waveform output is muxed onto a pin, which we never do. If HLFB PWM-duty capture is added later, use TC0–TC3 (all free in our design) via EVSYS.

---

## Session Log
*(append newest entries at the top: date, agent, what was done, what was learned, blockers)*

- **2026-08-04 — Claude (Phase 2 desk session, same day):** grblHAL core boots in the build: full core (44 files) compiling + linking against the pinned `306bf68d` master — **167.8 KB flash / 13.4 KB RAM**, vectors at 0x4000 verified. New: `driver.c`/`driver.h` (HAL v10; real stream/NVS/delay/motor-enables, safe stubs for motion/limits/coolant until Phases 3/4), `serial.c` (COM-0 as grblHAL stream with RT-command interception, replaces uart.c), `my_machine.h` (N_AXIS 4; force-included into every TU — core never includes it itself), `syscalls.c` (newlib stubs + real `_sbrk`; PlatformIO bare link ignores `--specs=`), NVMCTRL flash NVS in the last 8 KB block now carved out of the linker FLASH region. Learned: (a) Templates arm-driver targets HAL v8, ten versions stale — current SAM3X8E driver is the usable reference, exactly as PLAN §6.4 predicted; (b) one live API drift caught: `steppers.deenergize`→`energize`, and the core now applies motor energize itself at startup (grbllib.c:419); (c) SETTINGS_VERSION at this pin = 23 (hardcoded in driver_setup per org convention). References cloned: `/home/holla/grblHAL-Templates`, `/home/holla/grblHAL-SAM3X8E`, `/home/holla/grblHAL-SAMD21`. **Bench steps pending for owner (Phases 0+1+2 exit tests, in order):** (1) `make dump-bootloader` → verify non-blank → commit `bootloader-16k.bin`; (2) `make flash`; (3) underglow blinks ~1 Hz (SysTick+SR+clocks alive); (4) COM-0 per README wiring: **grblHAL banner on connect**, `$$` lists settings, change one + power-cycle + verify it stuck (proves flash NVS), `?` status, `0x18` soft reset; (5) `pio debug` breakpoint check; (6) double-tap reset → bootloader enumerates. Phase 3 (stepper engine — THE CRUX) is next desk work; step-path schematic verification is already done.

- **2026-08-04 — Claude (Phase 1 desk session, same day):** Phase 1 board-bring-up code written and building (4292 B flash): `clearcore.h` (pin map, SR bit masks + inversion constant 0xFFF300F3, clock/GPIO helpers), `shiftreg.c` (SERCOM6 SPI, Teknic protocol, SysTick-driven refresh, safe power-on defaults), `systick.c` (1 ms @ prio 7, millis/delay_ms, 1 Hz underglow blink for exit test), `uart.c` (SERCOM7 @ prio 4, ring buffers, echo console in main). system_same53.c trimmed to core clock path only; RESOURCES.md now holds all real claims. **Schematic netlist extraction (subagent, vector-geometry reconstruction of the Altium PDF) delivered three big results:** (a) shift-register sheet fully confirmed against Appendix A (4× SN74AHCT595, chain order, all 32 bits; ENn pulled up → outputs high-Z until firmware acts; SR_DATA_RET is a 5 V level into PC06 via only 2 kΩ — stock design, acceptable); (b) COM-0/COM-1 RJ-45 pinout extracted → README (sheet's PoE note has a 4-vs-7 pin typo, netlist wins); (c) **Phase 3 step-path question answered early: Mtr_CLK_01/PA27 + Mtr_CLK_23/PB23 are 74AHCT125 output-enables gating the B pins; drive them LOW as GPIOs and B pins are direct per-axis step sources; step/dir/HLFB all inverted through 74HC14 stages** — Phase 3 schematic-verification box checked, invert-mask defaults noted. **Bench steps pending for owner (Phase 0 + Phase 1 exit tests, in order):** (1) `make dump-bootloader`, verify non-blank, commit `bootloader-16k.bin`; (2) `make flash`; (3) underglow LED should blink at ~1 Hz (proves SysTick + shift register + 120 MHz clocks); (4) COM-0 RJ-45 echo test at 115200 per README wiring (proves UART + SR mode bits); (5) `pio debug`: breakpoint in main, SVD register view; (6) double-tap reset → bootloader still enumerates; (7) scope: 1 ms tick / GCLK if desired.

- **2026-08-04 — Claude (Phase 0 build session):** Initial commit of PLAN.md pushed (`f3d67b2`). **Owner switched project license to MIT mid-session** — LICENSE written, license statement added to plan preamble (firmware binary as a whole remains GPLv3 via the grblHAL core submodule; MIT is GPL-compatible, Phase 8 unaffected). All Phase 0 desk work done; **`pio run` builds green**: frameworkless PlatformIO skeleton (platformio.ini, cleaned bare-metal board JSON, SVD, OpenOCD cfgs, Makefile with flash/dump-bootloader/restore-bootloader), startup/system/ld vendored from Teknic 1.7.4 clone (re-cloned to `/home/holla/ClearCore-library`, still at pinned 4bf5ca6c = current master), grblHAL core submodule at `src/grbl` pinned `306bf68d` (filtered out of build until Phase 2), RESOURCES.md claim table, README with bootloader safety. Learned: (1) Teknic's real `Reset_Handler` is in SysManager.cpp and boots their library — wrote our own (data/bss + `SCB->VTOR` + SystemInit + libc + main); **Teknic never sets VTOR anywhere**. (2) **SAME53_DFP 3.x = Harmony-style headers, incompatible with everything this project cribs from; vendored 2.0.11, the newest ASF-style pack — never upgrade past 2.x.** (3) DFP 2.0.11 `DeviceVectors` uses per-source names; startup vector table renamed accordingly. Link verified: vectors at 0x4000, SP 0x20030000, entry 0x42b9. **Bench steps pending for owner:** (a) `make dump-bootloader` per board, verify non-blank, commit `bootloader-16k.bin`; (b) `make flash` the minimal main; (c) `pio debug` or gdb: halt in `main()`, watch `counter` increment; (d) double-tap reset → bootloader USB enumeration still works. These close the Phase 0 exit test.

- **2026-08-04 — Claude (planning session, hardware extraction):** Added Appendix A — full pin map extracted from library source (commit 4bf5ca6c): motor A/B/HLFB/enable per connector, the complete interrupt-capable-input list (only 7 general-purpose EXTINT inputs exist — IO-0..5 cannot interrupt), COM/SD/Ethernet/shift-register pins, 32-bit shift-register bit map. Two load-bearing discoveries: **motor enables are shift-register bits, not GPIO** (SR driver is motion-critical → Phase 1), and `Mtr_CLK_01/23` pins exist whose role in the step path must be verified from the schematic before first pulse (new Phase 3 task). Phase 3/4 tasks updated to reference Appendix A.

- **2026-08-04 — Claude (planning session, review pass):** Fable 5 adversarial review of this plan completed: every checkable hardware/post-mortem claim verified correct against source (15 `__disable_irq` sites exact, line numbers exact, all paths exist); 2 CRITICAL + 5 MAJOR + 9 MINOR process fixes applied (bootloader dump procedure, DFP sourcing correction, GPLv3 licensing task, master-SHA pinning, my_machine.h moved to Phase 2, frameworkless build mechanics, COM-0 physical layer, deprecated-API and attribution details). **Repo state: remote `origin` configured; ZERO commits — PLAN.md is untracked, first commit awaits owner instruction.**
- **2026-08-04 — Claude (planning session, later):** Created this repo (`/home/holla/grblhal-clearcore`, git init) and moved PLAN.md here from the 2021 archive repo per owner decision — the old repo is frozen as a salvage source only. Remote `git@github.com:holla2040/grblhal-clearcore.git` added and verified reachable (empty GitHub repo). First Phase 0 box checked.
- **2026-08-04 — Claude (planning session):** Research + this plan. Firmware survey (grblHAL wins), 2021 post-mortem (pulse disruption during 200 µs Teknic ISR; stepper HAL never written; TCC0/TCC1 timer collisions latent in driver.h), library source audit (v1.7.4: ~15 `__disable_irq()` sections; bursty step hardware confirmed), decision locked: bare-metal, zero Teknic runtime code. No implementation started.
