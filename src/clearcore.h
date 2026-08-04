/*
 * clearcore.h — board facts + tiny GPIO/clock helpers for grblhal-clearcore.
 * Pin map source: PLAN.md Appendix A (extracted from Teknic ClearCore-library
 * 1.7.4 HardwareMapping.h/ShiftRegister.h, cross-checked against schematic).
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef CLEARCORE_H
#define CLEARCORE_H

#include <sam.h>
#include <stdbool.h>
#include <stdint.h>

#define F_CPU_HZ 120000000UL

/* PORT group indices */
#define GRP_A 0
#define GRP_B 1
#define GRP_C 2

/* --- Clock helper macros (from Teknic libClearCore/inc/SysUtils.h, MIT) --- */

/* Wait for the synchronization bits (BITMASK) of the peripheral (PER). */
#define SYNCBUSY_WAIT(PER, BITMASK)                                            \
while ((PER)->SYNCBUSY.reg & (BITMASK)) {                                      \
    continue;                                                                  \
}

/* Enable the clock specified by the bit on the given peripheral bus. */
#define CLOCK_ENABLE(BUS, BIT)                                                 \
MCLK->BUS.bit.BIT = 1

/* Set a peripheral's GCLK source (SAMD5x/E5x datasheet 14.6.3.1/14.6.3.3). */
#define SET_CLOCK_SOURCE(PER_GCLK_ID, GCLK_INDEX)                              \
GCLK->PCHCTRL[(PER_GCLK_ID)].bit.CHEN = 0;                                     \
while (GCLK->PCHCTRL[(PER_GCLK_ID)].bit.CHEN) {                                \
    continue;                                                                  \
}                                                                              \
GCLK->PCHCTRL[(PER_GCLK_ID)].bit.GEN = GCLK_PCHCTRL_GEN((GCLK_INDEX));         \
GCLK->PCHCTRL[(PER_GCLK_ID)].bit.CHEN = 1;                                     \
while (GCLK->SYNCBUSY.reg & GCLK_SYNCBUSY_GENCTRL((GCLK_INDEX))) {             \
    continue;                                                                  \
}

/* --- GPIO helpers --- */

static inline void pin_dir_out(uint8_t grp, uint8_t pin)
{
    PORT->Group[grp].DIRSET.reg = 1UL << pin;
}

static inline void pin_write(uint8_t grp, uint8_t pin, bool high)
{
    if (high)
        PORT->Group[grp].OUTSET.reg = 1UL << pin;
    else
        PORT->Group[grp].OUTCLR.reg = 1UL << pin;
}

/* Route a pin to a peripheral function (0=A, 1=B, 2=C/SERCOM, 3=D/SERCOM_ALT, ...) */
static inline void pin_pmux(uint8_t grp, uint8_t pin, uint8_t func)
{
    if (pin & 1)
        PORT->Group[grp].PMUX[pin >> 1].bit.PMUXO = func;
    else
        PORT->Group[grp].PMUX[pin >> 1].bit.PMUXE = func;
    PORT->Group[grp].PINCFG[pin].bit.PMUXEN = 1;
}

/* --- Shift register (32-bit output chain) --- */

/* MCU pins (Appendix A): SERCOM6 SPI + GPIO strobe/enable */
#define SR_CLK_GRP      GRP_C   /* PC05 = SERCOM6 PAD1 (SCK), mux C */
#define SR_CLK_PIN      5
#define SR_DATA_GRP     GRP_C   /* PC07 = SERCOM6 PAD3 (DO), mux C */
#define SR_DATA_PIN     7
#define SR_DATA_RET_GRP GRP_C   /* PC06 = SERCOM6 PAD2 (DI), mux C */
#define SR_DATA_RET_PIN 6
#define SR_LOAD_GRP     GRP_B   /* PB02, GPIO strobe (latch on rising edge) */
#define SR_LOAD_PIN     2
#define SR_ENn_GRP      GRP_B   /* PB01, GPIO output-enable, active low */
#define SR_ENn_PIN      1

/* Bit masks (ShiftRegister.h:134-166, logical i.e. pre-inversion state) */
#define SR_A_CTRL_3         (1UL << 0)
#define SR_A_CTRL_2         (1UL << 1)
#define SR_LED_IO_5         (1UL << 2)
#define SR_LED_IO_4         (1UL << 3)
#define SR_LED_IO_3         (1UL << 4)
#define SR_LED_IO_2         (1UL << 5)
#define SR_LED_IO_1         (1UL << 6)
#define SR_LED_IO_0         (1UL << 7)
#define SR_EN_OUT_3         (1UL << 8)   /* motor M-3 enable */
#define SR_EN_OUT_2         (1UL << 9)   /* motor M-2 enable */
#define SR_EN_OUT_1         (1UL << 10)  /* motor M-1 enable */
#define SR_EN_OUT_0         (1UL << 11)  /* motor M-0 enable */
#define SR_UART_TTL_1       (1UL << 12)  /* COM-1 polarity: 0=TTL, 1=RS232 */
#define SR_UART_TTL_0       (1UL << 13)  /* COM-0 polarity: 0=TTL, 1=RS232 */
#define SR_UNDERGLOW        (1UL << 14)
#define SR_LED_USB          (1UL << 15)
#define SR_UART_SPI_SEL_1   (1UL << 16)  /* COM-1 mux: 0=UART, 1=SPI */
#define SR_UART_SPI_SEL_0   (1UL << 17)  /* COM-0 mux: 0=UART, 1=SPI */
#define SR_LED_COM_0        (1UL << 18)
#define SR_LED_COM_1        (1UL << 19)
#define SR_CFG00_AOUT       (1UL << 20)  /* IO-0 analog-out mode */
#define SR_LED_DI_6         (1UL << 21)
#define SR_LED_DI_7         (1UL << 22)
#define SR_LED_DI_8         (1UL << 23)
#define SR_LED_ADI_12       (1UL << 24)
#define SR_LED_ADI_11       (1UL << 25)
#define SR_LED_ADI_10       (1UL << 26)
#define SR_LED_ADI_09       (1UL << 27)
#define SR_ANAIN_DIGITAL_12 (1UL << 28)  /* A-12..A-9: 1=digital, 0=analog */
#define SR_ANAIN_DIGITAL_11 (1UL << 29)
#define SR_ANAIN_DIGITAL_10 (1UL << 30)
#define SR_ANAIN_DIGITAL_09 (1UL << 31)

/*
 * Hardware inversion mask (ShiftRegister.cpp constructor): every bit is
 * inverted on the wire EXCEPT the LEDs/enables/TTL-polarity bits listed
 * there (bits 2,3,8-15,18,19). Logical state ^ SR_INVERT = wire state.
 */
#define SR_INVERT 0xFFF300F3UL

/* --- COM-0 UART console (SERCOM7, RJ-45 jack, 5V TTL) --- */

#define COM0_TX_GRP  GRP_B      /* PB21 = SERCOM7 PAD0, mux D (TXPO=0) */
#define COM0_TX_PIN  21
#define COM0_RX_GRP  GRP_B      /* PB20 = SERCOM7 PAD1, mux D (RXPO=1) */
#define COM0_RX_PIN  20
#define COM0_PMUX_FUNC 3        /* function D */

#endif /* CLEARCORE_H */
