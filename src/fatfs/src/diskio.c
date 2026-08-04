/*
 * diskio.c — FatFs disk I/O layer: MMC/SDC over SPI for the ClearCore
 * microSD socket (physical drive 0, the only drive).
 *
 * The command sequencing, CSD arithmetic and timeout structure follow
 * ChaN's canonical "Generic MMC/SDC (in SPI mode) control module" sample
 * shipped with FatFs (http://elm-chan.org/fsw/ff/, ffsample.zip). That
 * sample is public-domain-equivalent: "This is a stand-alone module and
 * can be used without restriction." Adapted here to our polled SERCOM4
 * driver (sd_spi.c) and our 1 ms SysTick instead of the sample's
 * ISR-decremented timer variables.
 *
 * Only 512-byte sectors are supported (FF_MIN_SS == FF_MAX_SS == 512);
 * byte-addressed cards get CMD16 during initialisation to force that.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include <stddef.h>

#include "ff.h"
#include "diskio.h"

#include "sd_spi.h"
#include "systick.h"

/* MMC/SD command indices. 0x80 flags an application command (CMD55 first). */
#define CMD0    (0)             /* GO_IDLE_STATE */
#define CMD1    (1)             /* SEND_OP_COND (MMC) */
#define ACMD41  (0x80 + 41)     /* SEND_OP_COND (SDC) */
#define CMD8    (8)             /* SEND_IF_COND */
#define CMD9    (9)             /* SEND_CSD */
#define CMD12   (12)            /* STOP_TRANSMISSION */
#define ACMD13  (0x80 + 13)     /* SD_STATUS (SDC) */
#define CMD16   (16)            /* SET_BLOCKLEN */
#define CMD17   (17)            /* READ_SINGLE_BLOCK */
#define CMD18   (18)            /* READ_MULTIPLE_BLOCK */
#define ACMD23  (0x80 + 23)     /* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24   (24)            /* WRITE_BLOCK */
#define CMD25   (25)            /* WRITE_MULTIPLE_BLOCK */
#define CMD55   (55)            /* APP_CMD */
#define CMD58   (58)            /* READ_OCR */

/* Card type flags, kept in card_type */
#define CT_MMC      0x01        /* MMC v3 */
#define CT_SD1      0x02        /* SD v1 */
#define CT_SD2      0x04        /* SD v2+ */
#define CT_SDC      (CT_SD1 | CT_SD2)
#define CT_BLOCK    0x08        /* block addressing (SDHC/SDXC) */

#define INIT_TIMEOUT_MS  1000   /* ACMD41/CMD1 initialisation window */
#define RW_TIMEOUT_MS     500   /* card-busy wait after a write / before a cmd */
#define TOKEN_TIMEOUT_MS  200   /* data-token wait on a read */

static DSTATUS disk_stat = STA_NOINIT;
static BYTE card_type;

static inline bool elapsed(uint32_t start, uint32_t ms)
{
    return (millis() - start) >= ms;
}

/* Poll DO until the card releases it (reads 0xFF). 1 on ready, 0 on timeout. */
static int wait_ready(uint32_t timeout_ms)
{
    uint32_t start = millis();
    BYTE d;

    do {
        d = sd_spi_xfer(0xFF);
    } while (d != 0xFF && !elapsed(start, timeout_ms));

    return d == 0xFF;
}

/* Release CS and clock out one byte so the card lets go of DO. */
static void sd_deselect(void)
{
    sd_spi_cs(false);
    sd_spi_xfer(0xFF);
}

/* Assert CS and wait for the card to be ready. 1 on success. */
static int sd_select(void)
{
    sd_spi_cs(true);
    sd_spi_xfer(0xFF);

    if (wait_ready(RW_TIMEOUT_MS)) {
        return 1;
    }

    sd_deselect();

    return 0;
}

/* Receive a data block: token, len payload bytes, 2 CRC bytes (discarded). */
static int rcvr_datablock(BYTE *buff, UINT len)
{
    uint32_t start = millis();
    BYTE token;

    do {
        token = sd_spi_xfer(0xFF);
    } while (token == 0xFF && !elapsed(start, TOKEN_TIMEOUT_MS));

    if (token != 0xFE) {
        return 0;                       /* error token or no token at all */
    }

    sd_spi_read(buff, len);
    sd_spi_xfer(0xFF);                  /* discard CRC */
    sd_spi_xfer(0xFF);

    return 1;
}

#if FF_FS_READONLY == 0

/* Send a data block (or the 0xFD stop token of a multi-block write). */
static int xmit_datablock(const BYTE *buff, BYTE token)
{
    BYTE resp;

    if (!wait_ready(RW_TIMEOUT_MS)) {
        return 0;
    }

    sd_spi_xfer(token);

    if (token != 0xFD) {                /* 0xFD is stop-tran, no payload */
        sd_spi_write(buff, 512);
        sd_spi_xfer(0xFF);              /* dummy CRC */
        sd_spi_xfer(0xFF);

        resp = sd_spi_xfer(0xFF);       /* data response */
        if ((resp & 0x1F) != 0x05) {    /* 0x05 = accepted */
            return 0;
        }
    }

    return 1;
}

#endif /* FF_FS_READONLY == 0 */

/*
 * Send a command and return R1 (bit 7 set means no response). ACMDxx are
 * sent as CMD55 followed by the command itself.
 */
static BYTE send_cmd(BYTE cmd, DWORD arg)
{
    BYTE n, res;

    if (cmd & 0x80) {
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1) {
            return res;
        }
    }

    /* Re-select for every command except CMD12, which has to be sent while
       a multi-block read is still streaming */
    if (cmd != CMD12) {
        sd_deselect();
        if (!sd_select()) {
            return 0xFF;
        }
    }

    sd_spi_xfer(0x40 | cmd);
    sd_spi_xfer((BYTE)(arg >> 24));
    sd_spi_xfer((BYTE)(arg >> 16));
    sd_spi_xfer((BYTE)(arg >> 8));
    sd_spi_xfer((BYTE)arg);

    /* CRC is ignored in SPI mode except for the two commands the card still
       checks, which are the ones sent before CRC is switched off */
    n = 0x01;
    if (cmd == CMD0) {
        n = 0x95;                       /* valid CRC for CMD0(0) */
    }
    if (cmd == CMD8) {
        n = 0x87;                       /* valid CRC for CMD8(0x1AA) */
    }
    sd_spi_xfer(n);

    if (cmd == CMD12) {
        sd_spi_xfer(0xFF);              /* skip the stuff byte */
    }

    n = 10;
    do {
        res = sd_spi_xfer(0xFF);
    } while ((res & 0x80) && --n);

    return res;
}

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv ? STA_NOINIT : disk_stat;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    BYTE n, cmd, ty, ocr[4];
    uint32_t start;

    if (pdrv) {
        return STA_NOINIT;
    }

    sd_spi_init();                      /* idempotent; leaves the bus at 400 kHz */
    sd_spi_speed(false);

    /* 80 idle clocks with CS high put the card into native SPI mode */
    sd_spi_cs(false);
    for (n = 10; n; n--) {
        sd_spi_xfer(0xFF);
    }

    ty = 0;
    if (send_cmd(CMD0, 0) == 1) {                   /* enter idle state */

        start = millis();

        if (send_cmd(CMD8, 0x1AA) == 1) {           /* SDv2? */

            for (n = 0; n < 4; n++) {
                ocr[n] = sd_spi_xfer(0xFF);         /* trailing R7 bytes */
            }

            if (ocr[2] == 0x01 && ocr[3] == 0xAA) { /* 2.7-3.6 V range echoed back */

                /* ACMD41 with HCS set until the card leaves idle */
                while (!elapsed(start, INIT_TIMEOUT_MS) && send_cmd(ACMD41, 1UL << 30)) {
                    continue;
                }

                if (!elapsed(start, INIT_TIMEOUT_MS) && send_cmd(CMD58, 0) == 0) {
                    for (n = 0; n < 4; n++) {
                        ocr[n] = sd_spi_xfer(0xFF);
                    }
                    /* OCR bit 30 (CCS) distinguishes SDHC/SDXC from SDSC */
                    ty = (ocr[0] & 0x40) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
                }
            }

        } else {                                    /* SDv1 or MMCv3 */

            if (send_cmd(ACMD41, 0) <= 1) {
                ty = CT_SD1;
                cmd = ACMD41;
            } else {
                ty = CT_MMC;
                cmd = CMD1;
            }

            while (!elapsed(start, INIT_TIMEOUT_MS) && send_cmd(cmd, 0)) {
                continue;
            }

            if (elapsed(start, INIT_TIMEOUT_MS)) {
                ty = 0;
            }
        }

        /* Byte-addressed cards can come up with a block length other than
           512; block-addressed ones are fixed at 512 and reject CMD16 */
        if (ty && !(ty & CT_BLOCK) && send_cmd(CMD16, 512) != 0) {
            ty = 0;
        }
    }

    card_type = ty;
    sd_deselect();

    if (ty) {
        sd_spi_speed(true);
        disk_stat &= ~STA_NOINIT;
    } else {
        disk_stat = STA_NOINIT;
    }

    return disk_stat;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    DWORD sect = (DWORD)sector;

    if (pdrv || count == 0) {
        return RES_PARERR;
    }
    if (disk_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }

    if (!(card_type & CT_BLOCK)) {
        sect *= 512;                    /* byte address for SDSC/MMC */
    }

    if (count == 1) {
        if (send_cmd(CMD17, sect) == 0 && rcvr_datablock(buff, 512)) {
            count = 0;
        }
    } else {
        if (send_cmd(CMD18, sect) == 0) {
            do {
                if (!rcvr_datablock(buff, 512)) {
                    break;
                }
                buff += 512;
            } while (--count);

            send_cmd(CMD12, 0);
        }
    }

    sd_deselect();

    return count ? RES_ERROR : RES_OK;
}

#if FF_FS_READONLY == 0

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    DWORD sect = (DWORD)sector;

    if (pdrv || count == 0) {
        return RES_PARERR;
    }
    if (disk_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }

    if (!(card_type & CT_BLOCK)) {
        sect *= 512;
    }

    if (count == 1) {
        if (send_cmd(CMD24, sect) == 0 && xmit_datablock(buff, 0xFE)) {
            count = 0;
        }
    } else {
        if (card_type & CT_SDC) {
            /* Pre-erase hint; failure here is not fatal, only slower */
            send_cmd(ACMD23, count);
        }

        if (send_cmd(CMD25, sect) == 0) {
            do {
                if (!xmit_datablock(buff, 0xFC)) {
                    break;
                }
                buff += 512;
            } while (--count);

            if (!xmit_datablock(NULL, 0xFD)) {  /* stop-tran token */
                count = 1;
            }
        }
    }

    sd_deselect();

    return count ? RES_ERROR : RES_OK;
}

#endif /* FF_FS_READONLY == 0 */

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    DRESULT res = RES_ERROR;
    BYTE n, csd[16];
    DWORD csize;

    if (pdrv) {
        return RES_PARERR;
    }
    if (disk_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }

    switch (cmd) {

        case CTRL_SYNC:
            /* Nothing is buffered on our side; just wait out any programming
               the card is still doing so power can be cut safely */
            if (sd_select()) {
                res = RES_OK;
            }
            break;

        case GET_SECTOR_COUNT:
            if (send_cmd(CMD9, 0) == 0 && rcvr_datablock(csd, 16)) {
                if ((csd[0] >> 6) == 1) {       /* CSD v2 (SDHC/SDXC) */
                    csize = csd[9] + ((WORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
                    *(LBA_t *)buff = (LBA_t)csize << 10;
                } else {                        /* CSD v1 (SDSC/MMC) */
                    n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                    csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                    *(LBA_t *)buff = (LBA_t)csize << (n - 9);
                }
                res = RES_OK;
            }
            break;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            res = RES_OK;
            break;

        case GET_BLOCK_SIZE:                    /* erase block size, in sectors */
            if (card_type & CT_SD2) {
                if (send_cmd(ACMD13, 0) == 0) { /* SD status is 64 bytes */
                    sd_spi_xfer(0xFF);
                    if (rcvr_datablock(csd, 16)) {
                        for (n = 64 - 16; n; n--) {
                            sd_spi_xfer(0xFF);  /* purge the rest */
                        }
                        *(DWORD *)buff = 16UL << (csd[10] >> 4);
                        res = RES_OK;
                    }
                }
            } else {
                if (send_cmd(CMD9, 0) == 0 && rcvr_datablock(csd, 16)) {
                    if (card_type & CT_SD1) {
                        *(DWORD *)buff = (((csd[10] & 63) << 1) + ((WORD)(csd[11] & 128) >> 7) + 1)
                                         << ((csd[13] >> 6) - 1);
                    } else {                    /* MMC */
                        *(DWORD *)buff = ((WORD)((csd[10] & 124) >> 2) + 1) *
                                         (((csd[11] & 3) << 3) + ((csd[11] & 224) >> 5) + 1);
                    }
                    res = RES_OK;
                }
            }
            break;

        case MMC_GET_TYPE:
            *(BYTE *)buff = card_type;
            res = RES_OK;
            break;

        default:
            res = RES_PARERR;
            break;
    }

    sd_deselect();

    return res;
}
