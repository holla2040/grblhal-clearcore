/*
 * sd_spi.h — SERCOM4 SPI master for the ClearCore microSD socket.
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdbool.h>
#include <stdint.h>

/* Configure SERCOM4 + pins and leave the bus idle at card-identification
   speed with CS de-asserted. Safe to call more than once. */
void sd_spi_init(void);

/* false = 400 kHz (card identification), true = 20 MHz (data transfer).
   Never call with CS asserted: the SERCOM is disabled to reprogram BAUD. */
void sd_spi_speed(bool fast);

/* Drive CS: true = asserted (PA04 low), false = released (PA04 high). */
void sd_spi_cs(bool assert);

/* Full-duplex single byte. */
uint8_t sd_spi_xfer(uint8_t data);

/* Clock out 0xFF and store what comes back. */
void sd_spi_read(uint8_t *buf, uint32_t len);

/* Clock out buf, discard what comes back. */
void sd_spi_write(const uint8_t *buf, uint32_t len);

#endif /* SD_SPI_H */
