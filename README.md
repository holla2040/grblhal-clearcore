# grblhal-clearcore

Bare-metal [grblHAL](https://github.com/grblHAL/core) port for the Teknic
ClearCore controller (ATSAME53N19A, Cortex-M4F @ 120 MHz): a standalone
4-axis G-code motion controller driving ClearPath-SD servos over
step/direction. No RTOS, no Arduino, and **no Teknic library code executes
in this firmware** — see `PLAN.md` for the architecture and project plan.

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

## Flashing — READ THIS FIRST

The Teknic UF2/SAM-BA bootloader occupies flash `0x0000–0x4000` and **Teknic
does not publish it**. Losing it is the only irreversible failure mode on
this board.

Rules:

1. **NEVER chip-erase / mass-erase the part** — not from OpenOCD, not from
   MPLAB, not from any GUI "erase before program" checkbox.
2. Only flash through `make flash` (OpenOCD `program … 0x4000`), which
   sector-erases just the range it writes, starting at 0x4000.
3. **Before the first flash of each board, dump its bootloader:**

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
