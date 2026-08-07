# haasSender branch — bench verification, 2026-08-06/07

Board: ClearCore at 192.168.0.113, branch firmware 357,524 B (70.4% flash,
40% RAM): `N_TOOLS 32` + `NGC_EXPRESSIONS_ENABLE 1` in `src/my_machine.h`,
plus `src/haas_plugin.c` (M97). Flashed over the Atmel-ICE (`make flash`
with PlatformIO's openocd on PATH). All results below were taken with
EXCLUSIVE access — see "one client at a time", the lesson that cost an hour.

## Identity after flash

```
[OPT:VNML,100,1024,4,32]        ← trailing field = tool table size; sender detects it here
[NEWOPT:ENUMS,RT+,EXPR,TC,SED,ETH,mDNS,FS,SD]
[PLUGIN:HAAS parity v0.1]       ← the M97 plugin announcing itself
```

First boot after the flag change **wiped the NVS** (settings AND offsets) as
predicted — the layout moved for the tool table. Byte-exact pre-branch NVS is
in `debug/backups/nvs-8k-pre-haasSender.bin` (gitignored with the rest of
`debug/`; a second copy sits in `~/backups-haas-session/`). Restore = flash
main firmware, then write that block back at 0x7E000 over SWD. The wipe
repeats in BOTH directions of a main↔branch flash.

## Verified working (watched on the wire, DRO confirmed)

| Feature | Evidence |
|---|---|
| Tool table write | `G10 L1 P1 Z-25.4` → `[T:1\|…,-25.400,…]` in `$#` |
| **Persistence across power cycle** | relay off/on → `[T:1]` still −25.400 |
| Native `G43 H1` | `[TLO:…,-25.400,…]`; `G43 H2` (unset tool) → offset 0; `G49` clears |
| `T1 M6` | tool becomes 1 (`\|T:1\|` in status) |
| **M97 P100 L2 (the plugin)** | `$F=M97TEST.NC` ran the `N100` sub exactly twice: X 0→10.000, `[MSG:Pgm End]` |
| `G65 P100` / `M98 P100` | each ran `/P100.macro` (+2 Y per call) |
| O-words | `$F=OWORD.NC` with `o100 sub/call` ran (+1 Z) |
| Canned cycles | G81 (G98 return to initial Z watched), G82, G83, G85, G89, G73 all ran; G99 return verified |
| `$O` (inverted) | fresh boot: bare `M1` HOLDS (flag clear); `$O` → `Pn:T`, M1 passes; realtime 0x88 re-arms it |
| `$S` | `Pn:Q`; holds after EVERY block (each block behaves as M0); 0x89 clears |
| `$B` | `Pn:L`; `/G0 X77` skipped; off: slash consumed, block runs. Idle-only, no realtime byte |
| MACH3 `G51 X2` | `G0 X10` landed MPos 20.000, `\|Sc:` reported; `G50` clears |

## Corrections to the capability table

- **G51 is MACH3-style on this build** (`#define MACH3_SCALING`, gcode.c:50):
  the AXIS WORDS are the scale factors, scaling is about work zero, and the
  HAAS centre+P form answers error:36. The earlier "word sets match" note read
  the `#else` branch. The sender translates `G51 P<f>` → `G51 X<f> Y<f> Z<f>`
  and passes centre forms through to the honest error.
- **G86 answers error:28** (value word missing) with Z/R/F given — some
  additional word is required; not yet identified. G85 works.
- Quirk: a G51 that ERRORS (error:36) can still leave `Sc:` partially set —
  send `G50` after any failed G51.

## The error latch — pre-existing firmware bug, NOT a branch regression

**Symptom:** after at least one successful g-code block, a single errored
block (e.g. `G64` → error:20) makes EVERY subsequent g-code line answer the
SAME error code — across telnet reconnects and until a `$`-command runs.
`$G` clears it. Bisected on the bench across four builds: expressions off,
tool table off, plugin removed (= main-equivalent) — **all reproduce**, so it
ships in main today and is very likely what produced
`haasSender/history/g28-false-alarm.md` (two error:20s that "reproduced twice
and never again": G28 sent immediately after an errored line, and later
attempts happened to follow `$`-commands).

Nobody saw it before because haasSender sends `$G` after every manual command,
and a streamed job halts on its first error anyway. The sender now also sends
`$G` when the streamer halts on an error, so the next CYCLE START is never
poisoned. **Root cause in core not yet found** — the latch repeating the
original error code and `$`-commands clearing it points at a stored status
being replayed in the protocol/stream layer. Worth an upstream grblHAL issue
once minimised further.

## One client at a time

grblHAL multiplexes clients, and two senders driving one board produce
interleaved parser input: the first bench sweep returned error:20 for G54/G90
purely because another agent was streaming a job concurrently (its motion was
visible in unrequested `Run` reports). Telnet also CLAIMS the stream — a
websocket pendant shows `TELNET STREAM ACTIVE` while a telnet session is open.
Check for a second client (passively listen for unrequested status traffic)
before believing any bench result.

## Still untested

- `$341` tool-change modes 1–3 (M6 with $341=0 just sets the tool).
- G86's missing word; G84/G33/G76/G95 (need a spindle encoder — hardware).
- Homing/limit switches (no switches fitted, as ever).

## Cross-environment fixture (2026-08-07, addendum)

`haasSender/test/fixtures/O0100.nc` ran on the sim seat (streamed + expanded)
and via `$F=O0100.NC` DNC with `/P200.macro` on the card. Both ended at
**MPos [10.000, 2.000, -15.400]** — M97 P100 L2 twice, M98 P200 once, canned
cycles under the tool-table TLO.

Learned on the way:

- **In-file `T1 M6` enters the `Tool` state and waits for CYCLE START** (the
  expressions build's tool-change flow) — the HAAS-correct behaviour, and what
  the sim does. A streamed `T1 M6` over telnet just sets the tool.
- **A same-block `G49` does not lift the offset from its own move** —
  `G49 G0 Z10.` went to machine −15.400 (work Z10 under the old −25.4), while
  a same-block `G43 H` DOES shift its own move. The sim now mirrors both.
- **Closing the stream connection aborts a running `$F=` job.** The job dies
  with its session; hold the connection open to completion.
- **A job aborted mid-tool-change latches error:54** on every later G43/G49
  ("tool change pending"). A soft reset (0x18) clears it.
