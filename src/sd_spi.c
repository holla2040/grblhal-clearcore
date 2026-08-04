/*
 * sd_spi.c — SERCOM4 SPI master for the ClearCore microSD socket.
 *
 * Pin map from PLAN.md Appendix A; pad numbers and mux function read from
 * vendor/dfp/include/pio/same53n19a.h:
 *
 *   PB08 MOSI -> SERCOM4 PAD0, mux D   (PINMUX_PB08D_SERCOM4_PAD0)
 *   PB09 SCK  -> SERCOM4 PAD1, mux D   (PINMUX_PB09D_SERCOM4_PAD1)
 *   PB10 MISO -> SERCOM4 PAD2, mux D   (PINMUX_PB10D_SERCOM4_PAD2)
 *   PA04 CS   -> plain GPIO. PA04 has no SERCOM4 pad at all (its only SERCOM
 *                function is SERCOM0/PAD0), and SD needs CS held low across
 *                multi-command sequences anyway, which hardware SS cannot do.
 *
 * DO on PAD0 with SCK on PAD1 is DOPO=0; DI on PAD2 is DIPO=2.
 * SPI mode 0 (CPOL=0, CPHA=0), MSB first — what SD cards require in SPI mode,
 * and what Teknic's own driver selects (SdCardDriver.cpp: SpiClock(SCK_LOW,
 * LEAD_SAMPLE)).
 *
 * Polled, no interrupts, no DMA: the SD bus is touched only from the grbl
 * foreground loop, never from an ISR, so there is nothing to synchronise
 * against and the spin waits below are bounded by the SPI bit rate.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "clearcore.h"
#include "sd_spi.h"
#include "debug_uart.h"

#define SD_MOSI_GRP  GRP_B      /* PB08 = SERCOM4 PAD0 */
#define SD_MOSI_PIN  8
#define SD_SCK_GRP   GRP_B      /* PB09 = SERCOM4 PAD1 */
#define SD_SCK_PIN   9
#define SD_MISO_GRP  GRP_B      /* PB10 = SERCOM4 PAD2 */
#define SD_MISO_PIN  10
#define SD_CS_GRP    GRP_A      /* PA04, GPIO, active low */
#define SD_CS_PIN    4
#define SD_PMUX_FUNC 3          /* function D */

/*
 * BAUD = (fref / (2 * fbaud)) - 1 with fref = GCLK0 = 120 MHz.
 *   400 kHz -> 149   card identification (SD spec allows 100-400 kHz)
 *    20 MHz ->   2   data transfer
 * 20 MHz is above the SPI-master figure in the SAM E5x electrical
 * characteristics; if a card proves marginal on the bench, 15 MHz (BAUD 3)
 * and 12 MHz (BAUD 4) are the next steps down — change SD_SPI_HZ_FAST only.
 */
#define SD_SPI_HZ_SLOW  400000UL
#define SD_SPI_HZ_FAST  20000000UL
#define SD_SPI_CLK_HZ 1000000UL     /* GCLK5 for the clock-spec test */
#define SD_SPI_BAUD(hz) ((uint8_t)((SD_SPI_CLK_HZ / (2UL * (hz)) < 1UL) ? 0UL : (SD_SPI_CLK_HZ / (2UL * (hz)) - 1UL)))

void sd_spi_cs(bool assert)
{
    pin_write(SD_CS_GRP, SD_CS_PIN, !assert);
}

void sd_spi_speed(bool fast)
{
    SercomSpi *spi = &SERCOM4->SPI;

    spi->CTRLA.bit.ENABLE = 0;
    SYNCBUSY_WAIT(spi, SERCOM_SPI_SYNCBUSY_ENABLE);

    spi->BAUD.reg = fast ? SD_SPI_BAUD(SD_SPI_HZ_FAST) : SD_SPI_BAUD(SD_SPI_HZ_SLOW);

    spi->CTRLA.bit.ENABLE = 1;
    dbg_puts("sd: enable\n");
    SYNCBUSY_WAIT(spi, SERCOM_SPI_SYNCBUSY_ENABLE);
    dbg_puts("sd: enable done\n");
}

uint8_t sd_spi_xfer(uint8_t data)
{
    SercomSpi *spi = &SERCOM4->SPI;

    while (!spi->INTFLAG.bit.DRE) {
        continue;
    }
    spi->DATA.reg = data;

    while (!spi->INTFLAG.bit.RXC) {
        continue;
    }

    return (uint8_t)spi->DATA.reg;
}

/*
 * Both block helpers keep one byte in flight: the next byte is handed to
 * DATA as soon as the current one moves into the shift register, so the
 * clock never idles between bytes. At most one unread byte sits in the RX
 * buffer, so BUFOVF cannot trip.
 */
void sd_spi_read(uint8_t *buf, uint32_t len)
{
    SercomSpi *spi = &SERCOM4->SPI;

    if (len == 0) {
        return;
    }

    while (!spi->INTFLAG.bit.DRE) {
        continue;
    }
    spi->DATA.reg = 0xFF;

    while (--len) {
        while (!spi->INTFLAG.bit.DRE) {
            continue;
        }
        spi->DATA.reg = 0xFF;
        while (!spi->INTFLAG.bit.RXC) {
            continue;
        }
        *buf++ = (uint8_t)spi->DATA.reg;
    }

    while (!spi->INTFLAG.bit.RXC) {
        continue;
    }
    *buf = (uint8_t)spi->DATA.reg;
}

void sd_spi_write(const uint8_t *buf, uint32_t len)
{
    SercomSpi *spi = &SERCOM4->SPI;

    if (len == 0) {
        return;
    }

    while (!spi->INTFLAG.bit.DRE) {
        continue;
    }
    spi->DATA.reg = *buf++;

    while (--len) {
        while (!spi->INTFLAG.bit.DRE) {
            continue;
        }
        spi->DATA.reg = *buf++;
        while (!spi->INTFLAG.bit.RXC) {
            continue;
        }
        (void)spi->DATA.reg;
    }

    while (!spi->INTFLAG.bit.RXC) {
        continue;
    }
    (void)spi->DATA.reg;
}

void sd_spi_init(void)
{
    SercomSpi *spi = &SERCOM4->SPI;

    /* CS idle high before the pin becomes an output, so no glitch reaches
       the card while the SERCOM is still unconfigured */
    dbg_puts("sd: cs\n");
    pin_write(SD_CS_GRP, SD_CS_PIN, true);
    pin_dir_out(SD_CS_GRP, SD_CS_PIN);

    dbg_puts("sd: clocks\n");
    CLOCK_ENABLE(APBDMASK, SERCOM4_);
    /* TEST: 120 MHz GCLK0 exceeds the SERCOM 100 MHz core-clock spec —
       suspected cause of the enable-write CPU stall. GCLK5 = 1 MHz. */
    SET_CLOCK_SOURCE(SERCOM4_GCLK_ID_CORE, 5);
    dbg_puts("sd: clocks done\n");

    /* MISO needs the input buffer on, plus a pull-up so an empty socket
       reads 0xFF (card absent) instead of a floating level */
    PORT->Group[SD_MISO_GRP].OUTSET.reg = 1UL << SD_MISO_PIN;
    PORT->Group[SD_MISO_GRP].PINCFG[SD_MISO_PIN].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;

    pin_pmux(SD_MOSI_GRP, SD_MOSI_PIN, SD_PMUX_FUNC);
    pin_pmux(SD_SCK_GRP, SD_SCK_PIN, SD_PMUX_FUNC);
    pin_pmux(SD_MISO_GRP, SD_MISO_PIN, SD_PMUX_FUNC);

    dbg_puts("sd: swrst\n");
    spi->CTRLA.bit.SWRST = 1;
    SYNCBUSY_WAIT(spi, SERCOM_SPI_SYNCBUSY_SWRST);
    dbg_puts("sd: swrst done\n");

    /* SPI master, DO=PAD0/SCK=PAD1 (DOPO=0), DI=PAD2 (DIPO=2),
       MSB first, CPOL=0/CPHA=0 (mode 0) */
    spi->CTRLA.reg = SERCOM_SPI_CTRLA_MODE(0x3) |
                     SERCOM_SPI_CTRLA_DOPO(0x0) |
                     SERCOM_SPI_CTRLA_DIPO(0x2);
    spi->CTRLB.bit.RXEN = 1;
    dbg_puts("sd: ctrlb\n");
    SYNCBUSY_WAIT(spi, SERCOM_SPI_SYNCBUSY_CTRLB);
    dbg_puts("sd: ctrlb done\n");

    spi->BAUD.reg = SD_SPI_BAUD(SD_SPI_HZ_SLOW);

    spi->CTRLA.bit.ENABLE = 1;
    SYNCBUSY_WAIT(spi, SERCOM_SPI_SYNCBUSY_ENABLE);
}
