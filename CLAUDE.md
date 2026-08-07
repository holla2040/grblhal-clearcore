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
enumerates on a ttyACM port. Pick the port by descriptor, never by number —
other boards share the ttyACM range:

```
python3 -c "from serial.tools import list_ports; print([p.device for p in list_ports.comports() if 'ClearCore' in (p.product or '')])"
```

## Copying from the reference drivers

When lifting an idiom from SAM3X8E / iMXRT1062 / STM32F4xx, take the whole
idiom. Half of one compiles, boots and looks fine.

`settings_changed()` MUST act on the `changed` flags, not discard them. In
particular `changed.spindle` obliges the driver to re-run the spindle's
`config()` — `$30/$31/$33..$36` are written to `settings.pwm_spindle`
immediately, but the clamp in `spindle_set_rpm()` and the PWM gradient read
*copies* that only `spindle_precompute_pwm_values()` refreshes. Skip it and
the new limits silently do nothing until the next boot, which looks exactly
like the old "you must hard reset after a `$` change" folklore. Core marks
the settings that genuinely need a reboot with `.reboot_required = On`
(`$16`, spindle PPR); `$30` is not one of them.

All three reference drivers do this identically — see `spindle_cc_settings_changed()`
in `src/spindle.c`.

Bench check, no reset between steps: `$30=5000` then `M3 S6000` must report
`FS:0,5000`, not `FS:0,6000`.
