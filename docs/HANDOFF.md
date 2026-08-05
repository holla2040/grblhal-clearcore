# Handoff — grblHAL ClearCore bench session, night of 2026-08-05

Current state at the end of a long, productive bench session. **Read PLAN.md's
Session Log first** — the four newest entries cover today in detail and are not
duplicated here. This file covers only what those entries do *not*: the open
bug, the live board state, and where to pick up.

---

## 1. THE OPEN BUG — homing wedges the board hard

**Symptom.** During `$H`, the board frequently locks up completely. No `?`
response, no telnet, no USB, and the IO-0 debug console goes silent. Only a
physical reset recovers it. Reproduces "a lot" while hand-shorting a limit
jumper during the homing cycle.

**Why it is so total** (confirmed by reading `src/startup_same53.c`):

```c
void HardFault_Handler(void) __attribute__((weak, alias("Dummy_Handler")));
void Dummy_Handler(void) { while (1) { } }
```

Every fault handler is a weak alias to an infinite loop, and there are **no
`WDT->` writes anywhere in the driver** — so a fault is permanent and
completely silent. Nothing self-recovers.

**Two live hypotheses, not yet distinguished:**

| Hypothesis | Mechanism | Prediction |
|---|---|---|
| **A — prio-2 interrupt storm** | Homing is the only thing that arms the EIC limit interrupts (`limitsEnable()` → `EIC->INTENSET`). A hand-held jumper bounces filthily. If the EIC hardware debouncer (`DEBOUNCEN` + `DPRESCALER(7)\|STATES0` off 1 MHz GCLK5, nominally ~1.8 ms) is not filtering as assumed, prio-2 ISRs starve everything below them. | **Step pulses CONTINUE** during the wedge — the step ISR is prio 0 and preempts the storm. |
| **B — hard fault** | `limit_isr()`/`control_isr()` in `src/driver.c` call `hal.limits.interrupt_callback(...)` / `hal.control.interrupt_callback(...)` through **unguarded function pointers**. A NULL there is a wild jump to address 0. This project has already been bitten by exactly this — see the session-log entry about NULL `hal.enumerate_pins`. | **Step pulses STOP DEAD.** |

**THE DISCRIMINATING TEST — do this first, it costs nothing.**
The scope is already on a step pin (M-0 pin 2, ground on pin 6, or Combined I/O
as noted below). Trigger a wedge mid-seek and look: **pulses continuing means A,
pulses stopped means B.** Both look identical from the console, which is why the
console cannot tell you.

**Two fixes were proposed and agreed in principle but NOT written:**

1. **NULL guards** on the two EIC ISRs in `src/driver.c` (~1 line each). Correct
   regardless of root cause — it is an interrupt trust boundary and the project
   has form here. Will not fix hypothesis A.
2. **A real `HardFault_Handler`** that prints the stacked PC to the IO-0 debug
   console instead of spinning silently (~15 lines). This is the higher-value
   one: it converts every future wedge from a mystery into a self-diagnosing
   reproduction. The owner previously resorted to watchdog PC sampling for this
   same reason. If it prints on the next wedge, hypothesis B is confirmed and
   the PC names the culprit; if it stays silent while pulses continue, it is A.

Do not "fix" this by guessing. Two bugs today were solved by measuring
(TC6 register dump, raw `PORT IN` dump) after theory pointed the wrong way.

---

## 2. THE BOARD'S SETTINGS ARE NOT AT DEFAULTS

**Run `$$` before assuming anything.** Settings persist in NVS across reflashes,
and a lot were changed for bench tests. Values below are what was *asked for* —
several were never confirmed applied, so treat the list as "suspect these", not
"these are set".

| Setting | Bench value | Stock | Notes |
|---|---|---|---|
| `$0` | 2 | 10 | **Keep 2.** At 100 kHz the period is 10 µs, so `$0=10` fills the entire cycle and the step line sits high. ClearPath-SD needs ≥715 ns high AND low. |
| `$110`–`$113` | 24000, later 6000 | 500 | Raised for the 100 kHz jitter test, then again for homing seeks. **Revert to 500 for a real machine.** |
| `$120`–`$123` | 500 | 10 | Ramp, raised so cruise is reached in ~0.8 s not 40 s. Revert. |
| `$16` | 0 | 0 | Spindle invert. `0` = PWM duty tracks speed (VFD-signal wiring). `4` = complement (DRV8844 load wiring). Now actually functional — see PLAN.md. |
| `$22` | 1 | 0 | Homing enabled during this session. |
| `$44`/`$45`/`$46` | 1 / 0 / 0 | 4 / 3 / 0 | Reconfigured to home **X only**. Stock is Z first, then X+Y together. Unconfirmed whether applied. |
| `$25` / `$24` | 3000 / 200 | 500 / 25 | Homing seek/locate, raised so a seek is ~6 s not ~36 s. Unconfirmed. |
| `$27` | 25 | 1.0 | **Bench-specific and important.** With no motor, a 1 mm pull-off at 500 mm/min takes ~0.12 s — physically impossible to release a jumper inside that window, which raises ALARM:8. 25 mm buys ~3 s. **Revert to 1.0 for real switches.** |
| `$397` | 250 | 0 | WebUI auto-report interval, from an earlier session. |

Unchanged and worth knowing: `$5=15` (limits inverted, from `my_machine.h`),
`$21=0` (hard limits OFF), `$30=1000` (so `M3 S12000` clamps to 100% duty and is
useless as a PWM test — use S250/S750), `$100`–`$103=250` steps/mm,
`$130`–`$133=200` max travel.

---

## 3. Bench probe points (all verified working today)

- **Step pulses** — motor connector M-0 pin 2, return pin 6 (BLK/YEL on a
  ClearPath cable). X=M-0, Y=M-1, Z=M-2, A=M-3. Idles LOW, pulses HIGH.
- **Spindle PWM** — IO-4. On the Combined I/O header (10×2, 0.1"): **pin 18**,
  with GND on **pin 16**. Swings to Vsupply (24 V) — use a 10× probe.
- **Probe input** — IO-5, 3-position screw terminal labelled **S / G / +**.
  Short **S to G**. Needs 24 V present: its pull-up is 10 kΩ to Vsupply, so with
  no PSU the input floats low and reads permanently asserted.
- **Limits** — X=DI-6, Y=DI-7, Z=DI-8, A=A-9, same S/G/+ terminals, short S to G.
  These use a **different input circuit** — 5 kΩ pull-up to **5 V** — so unlike
  the probe they do **not** need 24 V.
- **Debug console** — IO-0 = Combined I/O **pin 12**, 115200 TX-only, relayed to
  UDP:8888. `dbg_puts()`/`dbg_hex32()`/`dbg_dec()`, compiled out by
  `DEBUG_CONSOLE_ENABLE 0`. This is the primary diagnostic channel and it earned
  its keep twice today.

Combined I/O header, for orientation (odd column left, even right):
```
A-9 1|2 A-10   A-11 3|4 GND    A-12 5|6 Vsup   DI-6 7|8 Vsup   DI-7 9|10 DI-8
I/O-1 11|12 I/O-0   I/O-2 13|14 Vsup   I/O-3 15|16 GND
I/O-4+ 17|18 I/O-4  I/O-5+ 19|20 I/O-5
```

---

## 4. Repo state

- Latest commit `1668514` — limits_override fix + spindle PWM invert fixes.
  **Committed but NOT pushed.**
- Working tree clean except `src/networking`, a submodule with uncommitted
  content inside it. **Leave it alone** — the owner confirmed this. The parent
  repo cannot capture edits made *inside* a submodule; that httpd duplicate-header
  fix is preserved as `tools/patches/plugin-networking-httpd-dup-headers.patch`.
- `*.uf2` is now gitignored; `make uf2` regenerates it.

---

## 5. Working rules (the owner is strict about these)

- **Flashing is a handshake.** `make uf2`, then STOP and tell the owner to
  double-press reset for DFU mode. He replies **"up"**. Only then
  `udisksctl mount -b /dev/sdb` and
  `cp grblhal-clearcore.uf2 /media/holla/CLEAR_BOOT/ && sync`. The copy itself
  reboots the board. Never probe for the drive before "up".
- **Never `git add -A` or `git add .`** — stage every path explicitly.
- **Never commit or push unless told.** "commit" ≠ "push". When he does say
  commit, commit *everything* (all changed and untracked files) — do not curate
  or ask file-by-file.
- **The `commit-review` skill must run before every `git commit`** and must be a
  separate Bash call from the commit itself, because the PreToolUse hook
  inspects the marker *before* the command runs.
- Commit trailer is exactly `Co-Authored-By: claude-opus-4-7 <noreply@anthropic.com>`.

---

## 6. Still blocked on hardware

Motors, power supply and cabling are in a box the owner has not located. So
ClearPath MSP configuration (800 steps/rev), physical motion, distance and
direction verification, and any real homing against actual switches all remain
untestable. Everything above was done motor-free.

**Remaining motor-free work**, once the wedge is understood:

- Finish `$H` on jumpers (blocked by the wedge).
- `$21=1` hard-limit behaviour — never exercised. Note the `limits_override` bug
  fixed today would have made hard limits silently ineffective, so this is now
  worth testing properly.
- Telnet **job streaming during a move** — the last piece of PLAN.md:138. Open
  streams were connected during today's jitter runs, but idle sockets are not
  the same load as a job being fed through one.
- Re-measure the step period during a 4-axis 100 kHz cruise. A gap-trigger
  cannot detect *uniform* rate reduction, so saturation would have been missed.
- Control inputs (reset / feed hold / cycle start) on A-10/A-11/A-12 — never
  tested, and they share the EIC path implicated in the wedge.
- `M62`/`M63` are **not testable** in this config: all 16 usable I/O are
  consumed, so no ioports are exposed. Not a defect.
- `hal.enumerate_pins` is still an empty stub — `$pins` returns nothing.
