/*
 * arch/cc.h — lwIP 2.1 compiler/platform abstraction for GCC ARM
 * (arm-none-eabi, Cortex-M4F) on grblhal-clearcore.
 *
 * lwIP's own lwip/arch.h already supplies GCC-correct defaults for the
 * integer typedefs, the printf formatters and the struct-packing macros, so
 * this header only fills in what has no sensible default: where errno comes
 * from, what a diagnostic or a failed assertion does on a machine with no
 * console, and where randomness comes from.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>

/* Use newlib's <errno.h> rather than lwIP's private error codes. */
#define LWIP_ERRNO_STDINCLUDE 1

/*
 * Diagnostics go nowhere: the serial ports belong to grblHAL's g-code
 * streams, and writing protocol chatter into them would corrupt a job.
 * Build with LWIP_DEBUG defined and a debugger attached to see anything.
 */
#define LWIP_PLATFORM_DIAG(x) do { } while (0)

#ifdef LWIP_DEBUG
#define LWIP_PLATFORM_ASSERT(x)                                                \
do {                                                                           \
    __asm volatile ("bkpt 0");                                                 \
    for (;;) { }                                                               \
} while (0)
#else
/*
 * A failed lwIP assertion must not stop the machine. Spinning here would
 * strand a cutter in the work with the spindle running, which is a far worse
 * outcome than a dropped packet, so release builds carry on and let the
 * network stack fail on its own terms.
 */
#define LWIP_PLATFORM_ASSERT(x) do { (void)(x); } while (0)
#endif

/*
 * Randomness for DHCP transaction IDs and TCP initial sequence numbers.
 * Defined in ethernetif.c, seeded from the MAC address and the millisecond
 * tick so two ClearCores on one network do not march in lockstep.
 */
uint32_t lwip_rand(void);
#define LWIP_RAND() ((u32_t)lwip_rand())

#endif /* LWIP_ARCH_CC_H */
