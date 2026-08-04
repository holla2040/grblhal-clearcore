/*
 * shiftreg.c — ClearCore 32-bit output shift-register chain driver.
 *
 * Protocol adapted from Teknic-Inc/ClearCore-library (tag 1.7.4, MIT)
 * libClearCore/src/ShiftRegister.cpp: SERCOM6 as SPI master, 32-bit
 * LSB-first transfers, SCK = GCLK5 (1 MHz) / 2 = 500 kHz, GPIO strobe on
 * SR_LOAD latches the PREVIOUS transfer into the output registers, SR_ENn
 * gates the chain outputs. Changes: bare-metal C, no LED blink patterns,
 * refresh driven from our 1 ms SysTick instead of Teknic's 5 kHz tick.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "clearcore.h"
#include "shiftreg.h"

static volatile uint32_t sr_state;      /* logical (pre-inversion) state */
static volatile bool sr_ready;

void sr_set(uint32_t mask)
{
    __atomic_fetch_or(&sr_state, mask, __ATOMIC_RELAXED);
}

void sr_clear(uint32_t mask)
{
    __atomic_fetch_and(&sr_state, ~mask, __ATOMIC_RELAXED);
}

void sr_toggle(uint32_t mask)
{
    __atomic_fetch_xor(&sr_state, mask, __ATOMIC_RELAXED);
}

uint32_t sr_get(void)
{
    return sr_state;
}

/*
 * Latch the word shifted out on the previous call, then start shifting the
 * current state. Called from SysTick (1 kHz): the 64 us SPI transfer is
 * long finished by the next call, so the flag waits below never spin in
 * steady state.
 */
void sr_refresh(void)
{
    SercomSpi *spi = &SERCOM6->SPI;

    if (!sr_ready) {
        return;
    }

    if (!spi->INTFLAG.bit.TXC) {
        return;                          /* previous transfer still on the wire */
    }

    /* Rising edge on LOAD latches the previously shifted word */
    pin_write(SR_LOAD_GRP, SR_LOAD_PIN, true);
    pin_write(SR_LOAD_GRP, SR_LOAD_PIN, false);

    /* Drain the readback word so RX never overflows */
    if (spi->INTFLAG.bit.RXC) {
        (void)spi->DATA.reg;
    }

    spi->DATA.reg = sr_state ^ SR_INVERT;
}

void sr_init(void)
{
    SercomSpi *spi = &SERCOM6->SPI;

    /* Outputs disabled while the chain contents are undefined */
    pin_write(SR_ENn_GRP, SR_ENn_PIN, true);
    pin_write(SR_LOAD_GRP, SR_LOAD_PIN, false);
    pin_dir_out(SR_ENn_GRP, SR_ENn_PIN);
    pin_dir_out(SR_LOAD_GRP, SR_LOAD_PIN);

    CLOCK_ENABLE(APBDMASK, SERCOM6_);
    SET_CLOCK_SOURCE(SERCOM6_GCLK_ID_CORE, 5);      /* GCLK5 = 1 MHz */

    pin_pmux(SR_CLK_GRP, SR_CLK_PIN, 2);            /* SERCOM6 PAD1, func C */
    pin_pmux(SR_DATA_GRP, SR_DATA_PIN, 2);          /* SERCOM6 PAD3, func C */
    pin_pmux(SR_DATA_RET_GRP, SR_DATA_RET_PIN, 2);  /* SERCOM6 PAD2, func C */

    spi->CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(spi, SERCOM_SPI_SYNCBUSY_SWRST);

    /* SPI master, DO=PAD3/SCK=PAD1 (DOPO=2), DI=PAD2 (DIPO=2), LSB first */
    spi->CTRLA.reg = SERCOM_SPI_CTRLA_MODE(0x3) |
                     SERCOM_SPI_CTRLA_DOPO(0x2) |
                     SERCOM_SPI_CTRLA_DIPO(0x2) |
                     SERCOM_SPI_CTRLA_DORD;
    spi->CTRLB.bit.RXEN = 1;
    spi->CTRLC.reg = SERCOM_SPI_CTRLC_DATA32B;      /* 32-bit DATA register */
    spi->BAUD.reg = 0;                              /* SCK = 1 MHz / 2 */

    spi->CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(spi, SERCOM_SPI_SYNCBUSY_ENABLE);

    /*
     * Power-on defaults: motors disabled, COM-0/1 in TTL UART mode
     * (UART_TTL_x = 0, UART_SPI_SEL_x = 0), A-9..A-12 in digital mode so
     * they are usable as limit/control inputs, underglow on as an
     * "our firmware is alive" indicator.
     */
    /* underglow starts OFF — the blink telemetry owns it from tick 1 */
    sr_state = SR_ANAIN_DIGITAL_09 | SR_ANAIN_DIGITAL_10 |
               SR_ANAIN_DIGITAL_11 | SR_ANAIN_DIGITAL_12;

    /* Shift the initial word, latch it, shift it again, then enable outputs
       (mirrors Teknic's Initialize() sequence) */
    spi->DATA.reg = sr_state ^ SR_INVERT;
    while (!spi->INTFLAG.bit.TXC) { }
    sr_ready = true;
    sr_refresh();
    while (!spi->INTFLAG.bit.TXC) { }

    pin_write(SR_ENn_GRP, SR_ENn_PIN, false);       /* outputs live */
}
