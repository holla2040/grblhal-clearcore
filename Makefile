# grblhal-clearcore flash/debug recipes, adapted from the 2021
# clearcore_projects/src/grbl_clearcore Makefile (OpenOCD invocations kept).
#
# SAFETY: the Teknic bootloader lives at 0x0000-0x4000 and Teknic does not
# publish it. NEVER mass-erase / chip-erase this part. The `program` command
# below sector-erases only the range it writes, starting at 0x4000.
# Run `make dump-bootloader` (and commit bootloader-16k.bin) BEFORE the
# first flash of each board.

BUILD = .pio/build/clearcore

bin:
	pio run

# .bin -> .uf2 for the drag-drop path: double-tap the ClearCore reset
# button, a boot drive appears, copy grblhal-clearcore.uf2 onto it.
uf2: bin
	python3 tools/bin2uf2.py -o grblhal-clearcore.uf2 $(BUILD)/firmware.bin

# Flash over USB via the Teknic bootloader (bossac/SAM-BA, 1200-baud touch,
# app at 0x4000). Bootloader-safe by design — the bootloader does the
# writing and protects its own 16 KB. No debugger needed.
flash-usb: bin
	pio run -t upload

# Flash via Atmel-ICE/SWD (sector erase, app range only)
flash: bin
	openocd -f clearcore.cfg -c "program $(BUILD)/firmware.bin verify reset 0x4000;shutdown"

reset:
	openocd -f clearcore.cfg -c "init;reset;shutdown"

# gdb server for manual debugging (pio debug is the usual path)
serve:
	openocd -f clearcore-debug.cfg

debug:
	arm-none-eabi-gdb $(BUILD)/firmware.elf

# The only irreversible failure mode in this project is losing the
# bootloader. Dump it first, verify it is not blank, keep it in git.
dump-bootloader:
	openocd -f clearcore.cfg -c "init; dump_image bootloader-16k.bin 0x0 0x4000; shutdown"
	@if [ "$$(tr -d '\377' < bootloader-16k.bin | wc -c)" -eq 0 ]; then \
		echo "WARNING: dump is all 0xFF — flash blank or read failed; DO NOT trust this file"; \
	else \
		echo "bootloader-16k.bin dumped, non-blank — commit it"; \
	fi

# Recovery: restore a previously dumped bootloader (writes 0x0-0x4000 only)
restore-bootloader:
	openocd -f clearcore.cfg -c "program bootloader-16k.bin verify 0x0;shutdown"

clean:
	pio run -t clean
