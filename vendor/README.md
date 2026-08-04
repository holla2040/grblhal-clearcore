# Vendored MCU headers

Headers-only vendor tree for bare-metal PlatformIO builds targeting the
ATSAME53N19A (Cortex-M4F). No Arduino/Atmel Studio framework is used, so
CMSIS core and device headers are carried in-repo.

Vendored on 2026-08-04, DFP version revised on 2026-08-04 (same day —
see "Why the old register style" below).

## vendor/dfp — Microchip SAME53 Device Family Pack

- Pack: `Microchip.SAME53_DFP`
- Version: `2.0.11` (deliberately **not** latest — see rationale below)
- Source: https://packs.download.microchip.com/Microchip.SAME53_DFP.2.0.11.atpack
- Pack index: https://packs.download.microchip.com/Microchip.SAME53_DFP.pdsc
- License: Apache License 2.0. This pack version ships no standalone
  license file, so `vendor/dfp/LICENSE.txt` is assembled from the
  identical Apache-2.0 SPDX notice that appears verbatim in the banner of
  every header in the tree (see that file for the exact wording and
  source line it was taken from).

### Why the old register style

DFP versions were vendored twice. The first pass used `3.10.248` (the
newest release), which turned out to use Microchip's newer
**Harmony-style** register headers: peripherals are accessed through a
`_REGS` pointer to a `_registers_t` struct (e.g.
`GCLK_REGS->GCLK_GENCTRL[n]`), with no `DeviceVectors` typedef in the
form this project's code expects. This project's driver code and the
grblHAL/ASF-style reference sources it is written against instead expect
the **old CMSIS-Atmel / ASF style**: peripherals accessed via a
struct-pointer macro (e.g. `#define GCLK ((Gclk *)0x40001C00UL)`), with
register-union members addressable as both `.reg` and `.bit.<field>`,
and array-style register blocks such as `GCLK->GENCTRL[n].reg`. Building
against `3.10.248` failed with errors like `GCLK undeclared`, `CMCC
undeclared`, and `GCLK_GENCTRL_OE undeclared`.

Every `3.x` release available from the pack index (`3.10.248` down
through `3.0.25`) was checked and all of them use the Harmony/`_REGS`
style — the switch to Harmony-style headers happened at or before
`3.0.25`, not specifically at `3.10`. The pack index's next (and last)
listed release below `3.0.25` is `2.0.11`, which was checked and
confirmed old-style on every count that matters for this project:

- `typedef struct _DeviceVectors { ... } DeviceVectors;` is present.
- `#define GCLK ((Gclk *)0x40001C00UL)` — struct-pointer style, not
  `GCLK_REGS`.
- `component/gclk.h` defines `GCLK_GENCTRL_OE`; `component/cmcc.h`
  defines `CMCC_CTRL_CEN`.
- `Gclk.GENCTRL` is a `GCLK_GENCTRL_Type GENCTRL[12]` array (so
  `GCLK->GENCTRL[1].reg` resolves), and `Gclk.SYNCBUSY` is a plain
  register (so `GCLK->SYNCBUSY.reg` resolves).
- `Mclk.APBAMASK` is a register union with a `.bit.SERCOM0_` field.

`2.0.11` is therefore the newest pack release that is old-style, and was
selected. (Its changelog entry notes it "Succeeds Atmel.SAME53_DFP
1.0.75" — an even older Atmel-branded release exists upstream but was
not needed once `2.0.11` checked out clean; `Atmel.SAME53_DFP.pdsc`
also 403s on the current pack repo, so it wasn't readily fetchable
anyway.)

Versions tried and rejected (all confirmed Harmony/`_REGS` style):
`3.10.248`, `3.9.238`, `3.8.235`, `3.7.228`, `3.6.115`, `3.5.87`,
`3.4.79`, `3.3.66`, `3.2.49`, `3.1.38`, `3.0.25`.

### What was extracted

The pack's `include/` directory in full (`same53.h`, `sam.h`,
`same53j18a.h`, `same53j19a.h`, `same53j20a.h`, `same53n19a.h`,
`same53n20a.h`, the single shared `system_same53.h`,
`component-version.h`, and the `component/`, `instance/`, `pio/`
subdirectories). The directory was already headers-only in the pack (no
atdf/svd/docs/toolchain-support files live under `include/`), so it was
copied as-is to preserve the pack's internal include layout. Everything
else in the pack (`atdf/`, `svd/`, `edc/`, `gcc/`, `armcc/`, `iar/`,
`keil/`, `include_mcc/`, `scripts/`, `templates/`, `package.content`)
was left out.

### Dispatch header

This pack version ships **both** `same53.h` and `sam.h` as top-level
convenience headers, and they are functionally identical — both dispatch
to the correct `same53<variant>.h` based on a `__SAME53xxxx__` /
`__ATSAME53xxxx__` device define. This project's sources already do
`#include <sam.h>`, which resolves correctly with this pack; **no
changes to `src/` were needed**. For the N19A variant, define
`__SAME53N19A__` (or `__ATSAME53N19A__`).

## vendor/cmsis — ARM CMSIS Core headers

- Pack: `ARM.CMSIS`
- Version: `6.3.0` (latest available at vendor time)
- Source: https://packs.download.microchip.com/ARM.CMSIS.6.3.0.atpack
  (ARM.CMSIS is mirrored on the Microchip pack repo; upstream is
  https://github.com/ARM-software/CMSIS_5)
- Pack index: https://packs.download.microchip.com/ARM.CMSIS.pdsc
- License: Apache License 2.0 — `vendor/cmsis/LICENSE` (copied verbatim
  from the pack root)

Extracted from `CMSIS/Core/Include/`: only the files needed to satisfy
`#include <core_cm4.h>` when built with `arm-none-eabi-gcc` for a
Cortex-M4(F):

- `core_cm4.h`
- `cmsis_version.h`
- `cmsis_compiler.h`
- `cmsis_gcc.h`
- `m-profile/armv7m_mpu.h`
- `m-profile/cmsis_gcc_m.h`

In CMSIS 6.x the MPU header was renamed/relocated from the older
`mpu_armv7.h` to `m-profile/armv7m_mpu.h`, and the GCC intrinsics split
out into `m-profile/cmsis_gcc_m.h`; both are pulled in transitively by
`core_cm4.h` -> `cmsis_compiler.h` -> `cmsis_gcc.h` for this toolchain.
`<arm_acle.h>`, included by `cmsis_gcc.h`, is supplied by the
arm-none-eabi-gcc toolchain itself and is not vendored. Everything else
in the CMSIS pack (RTOS2, Driver, Documentation, other core_*.h variants,
a-profile/r-profile headers) was left out.

**Compatibility with old-style DFP 2.0.11**: `same53n19a.h` in the
2.0.11 pack does `#include <core_cm4.h>` (unconditional angle-bracket
include, same as newer DFPs), which resolves cleanly against the
CMSIS 6.3.0 headers above — the old-style ASF register definitions and
the newer CMSIS core headers don't overlap in scope (CMSIS core only
defines `SCB`, `NVIC`, intrinsics, `IRQn_Type`, etc.; the device pack
defines the peripheral structs/macros), so there was no need to roll
back to an older CMSIS. Confirmed by the compile test below, which
exercises `SCB->VTOR` (a CMSIS-core symbol) alongside DFP peripheral
struct access in the same translation unit. No CMSIS 5.x fallback was
required.

## Using both trees together

A single pair of `-I` flags resolves all includes:

```
-I<repo>/vendor/dfp/include -I<repo>/vendor/cmsis/include
```

Compile-tested against old-style access patterns representative of this
project's driver code:

```c
#include <sam.h>
int main(void){
  GCLK->GENCTRL[1].reg = GCLK_GENCTRL_GENEN | GCLK_GENCTRL_OE;
  CMCC->CTRL.reg = CMCC_CTRL_CEN;
  SCB->VTOR = 0;
  MCLK->APBAMASK.bit.SERCOM0_ = 1;
  return (int)GCLK->SYNCBUSY.reg;
}
```

```
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -Wall -Wextra \
  -D__SAME53N19A__ -D__SAME53__ \
  -Ivendor/dfp/include -Ivendor/cmsis/include \
  test.c -o test.o
```

using the xPack `arm-none-eabi-gcc 12.3.1` from
`~/.platformio/packages/toolchain-gccarmnoneeabi/bin/`. Result: clean
compile, zero warnings, zero errors. Disassembly of the resulting object
confirms real register stores at the expected peripheral base addresses
(e.g. `0x40001c00` for `GCLK`), not stubbed-out/optimized-away code.
