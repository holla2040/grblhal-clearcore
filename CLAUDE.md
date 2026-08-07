# grblHAL ClearCore

## Power

The ClearCore is powered through a network relay at `relay1.local`.

Turn it on:

```
curl -s -m 10 http://relay1.local/on
```

## Flash

Flash via the Atmel-ICE (SWD) — no double-tap / CLEAR_BOOT drive needed:

```
PATH="$HOME/.platformio/packages/tool-openocd/bin:$PATH" make flash
```

openocd is not on the system PATH; PlatformIO's build (0.12) works. Writes the
app at 0x4000 with sector erase only — NEVER mass-erase, the Teknic bootloader
at 0x0–0x4000 is unpublished. Run `make dump-bootloader` before the first
flash of a new board.

Verify after flash: `clearcore.local` resolves and "Teknic ClearCore_grblHAL"
enumerates on a ttyACM port.
