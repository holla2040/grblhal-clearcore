# grblhal-clearcore

Bare-metal [grblHAL](https://github.com/grblHAL/core) port for the Teknic
ClearCore controller (ATSAME53N19A, Cortex-M4F @ 120 MHz): a standalone
4-axis G-code motion controller driving ClearPath-SD servos over
step/direction. No RTOS, no Arduino, and **no Teknic library code executes
in this firmware** — see `PLAN.md` for the architecture and project plan.

## Status

All phases (0–7) are desk-complete and building — 313 KB flash / 75 KB
RAM. **Bench validation pending** (checklist in PLAN.md's Session Log).

| Subsystem | State |
|---|---|
| 4-axis step engine (TC4+TC5 + TC6, prio 0) | built, awaiting scope |
| Homing/limits/control (EIC prio 2, hw debounce) + polled probe | built |
| Motor enables + LEDs + port modes (shift register) | built |
| PWM spindle (DRV8844/IO-4) + coolant (IO-3/IO-0) | built |
| Settings NVS (last 8 KB flash block) | built |
| UART console (COM-0) + USB CDC (TinyUSB) | built |
| Ethernet: telnet/websocket/HTTP + WebUI (GMAC + lwIP 2.2) | built |
| SD job streaming + G65 macros (SERCOM4 + FatFs) | built |

Configuration lives in `src/my_machine.h` (**run `pio run -t clean`
after changing it** — SCons cannot see force-included headers).

License: MIT (this repo's files). The grblHAL core submodule is GPLv3, so
the linked firmware binary as a whole is distributed under GPLv3 terms.
See `LICENSE`.

## Build

Requires [PlatformIO](https://platformio.org) on Linux. Bare build, no
framework — startup/vector/clock code is in `src/`, device headers are
vendored in `vendor/`.

```
pio run          # or: make bin
```

Output: `.pio/build/clearcore/firmware.{elf,bin}`, linked at 0x4000.

## Flashing

**No debugger needed.** The Teknic bootloader (first 16 KB) offers two USB
paths, both bootloader-safe by design — the bootloader itself does the
writing and protects its own region:

1. **Drag-drop (simplest):** `make uf2`, double-tap the ClearCore reset
   button (a USB boot drive appears), copy `grblhal-clearcore.uf2` onto
   it. Board resets into grblHAL. While you're there, save the drive's
   `INFO_UF2.TXT` contents into PLAN.md — it identifies the bootloader.
2. **bossac (scriptable):** `make flash-usb` — PlatformIO's sam-ba upload
   with automatic 1200-baud touch (Teknic's own `flash_clearcore.cmd`
   flow: app port 2890:8022 → touch → bootloader port 2890:0022 →
   `bossac --offset=0x4000`). Once grblHAL runs, its CDC also honors the
   1200-baud touch, so reflashing needs no button presses.
3. **Atmel-ICE/SWD:** `make flash` (OpenOCD, sector-erase, 0x4000 up).
   Only needed for debugging or bootloader recovery.

### Bootloader safety (SWD path only)

The bootloader in `0x0000–0x4000` is not published by Teknic — losing it
is the only irreversible failure mode, and only SWD tools can touch that
region. If you use the Atmel-ICE: **never chip-erase / mass-erase**, and
dump the bootloader first, once per board:

   ```
   make dump-bootloader     # writes + sanity-checks bootloader-16k.bin
   git add bootloader-16k.bin && git commit
   ```

   The target warns if the dump reads all-0xFF (blank/failed read) — do not
   trust such a dump.

### Bootloader recovery

If the bootloader is ever damaged and you have the dump:

```
make restore-bootloader    # openocd program bootloader-16k.bin verify 0x0
```

The double-tap-reset bootloader entry and USB enumeration should work again
afterwards.

## COM-0 console wiring (RJ-45)

COM-0 is the firmware console: 115200-8N1, **5 V TTL** — use a 5 V-tolerant
USB-UART adapter. Pinout (schematic page 9, netlist-verified; COM-1 is
identical):

| RJ-45 pin | Signal | Direction |
|---|---|---|
| 1 | RTS | out of ClearCore (unused for now) |
| 2 | +5 V | power out |
| 3 | CTS | into ClearCore (unused for now) |
| 4 | GND | — |
| 5 | TX | out of ClearCore |
| 6 | +5 V | power out |
| 7 | GND | — |
| 8 | RX | into ClearCore |

Minimal hookup: adapter RX → pin 5, adapter TX → pin 8, GND → pin 4 or 7.
Signals are non-inverted TTL, idle high. Board pull-ups/downs default the
port to TTL UART mode even before firmware runs; `sr_init()` keeps it there
via the shift-register mode bits.

(The schematic's own page-9 note claims the PoE-protection GND short is on
pins "4 and 8" — the netlist says pins 4 and 7. Pin 8 is RX. Trust the
netlist.)

## Debug

Atmel-ICE on SWD. `pio debug`, or manually: `make serve` (OpenOCD gdb
server) + `make debug` (gdb on the elf). Board configs: `clearcore.cfg`
(one-shot flash/reset) and `clearcore-debug.cfg` (server stays up).

## Layout

| Path | What |
|---|---|
| `PLAN.md` | The working plan — read first; progress tracker + session log |
| `RESOURCES.md` | Peripheral/IRQ/GCLK claim table — every claim is recorded |
| `src/` | Firmware source (startup, system/clock init, main) |
| `src/grbl/` | grblHAL core (git submodule, pinned) |
| `ld/` | Linker script (app at 0x4000, after bootloader) |
| `boards/`, `debug/` | PlatformIO board JSON, SVD |
| `vendor/` | CMSIS + Microchip SAME53 DFP headers (see vendor/README.md) |
