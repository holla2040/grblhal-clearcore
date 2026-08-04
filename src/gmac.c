/*
 * gmac.c — bare-metal SAME5x GMAC + KSZ8081 RMII PHY driver for
 * grblhal-clearcore.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 *
 * ---------------------------------------------------------------------------
 * Adapted from Teknic ClearCore-library 1.7.4 (commit 4bf5ca6c), MIT:
 *   libClearCore/src/EthernetManager.cpp   (GMAC + PHY register sequences)
 *   libClearCore/inc/EthernetApi.h         (descriptor layouts, ring sizes)
 *   libClearCore/inc/Phy.h                 (KSZ8081 register map)
 *   libClearCore/src/NvmManager.cpp        (MAC address in the NVM user page)
 *   LwIP/LwIP/port/include/ethernetif.c    (ring walking)
 *
 * Copyright (c) 2020 Teknic, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Changes from the Teknic original:
 *   - C++ singleton class -> plain C with file-static state; no dynamic
 *     allocation anywhere.
 *   - Their GMAC ISR set a "frame received" flag and their PHY interrupt ran
 *     MDIO transactions from EXTINT12 (PC28). We claim neither PC28 nor an EIC
 *     line: GMAC_Handler only acknowledges hardware status, and the link is
 *     polled by gmac_link_up() from the foreground. Their IrqHandlerPhy
 *     speed/duplex re-match is ported into that poll.
 *   - Frame in/out replaces their PacketLength/PacketRead/PacketWrite. Same
 *     ring walk, but: descriptors skipped over (receive-error fragments,
 *     datasheet 24.6.3.3) are released instead of leaked, so the receive ring
 *     cannot jam; the transmit path tracks its own in-flight count instead of
 *     rediscovering it, and never blocks waiting for a descriptor.
 *   - Received frames are handed up with the 4-byte FCS removed. Teknic left
 *     GMAC->NCFGR.RFCS clear (FCS stored in the buffer and counted in the
 *     descriptor length) and passed the trailing FCS on to lwIP.
 *   - MDIO transactions are bounded; Teknic's spin on NSR.IDLE has no exit.
 *     A dead PHY must not wedge a motion controller at boot.
 *   - Explicit 8-byte alignment on descriptors and buffers. The GMAC takes the
 *     buffer address from bits 31:2 of the descriptor, so a misaligned buffer
 *     silently loses its low address bits; Teknic's were class members with no
 *     stated alignment.
 *   - MAC address bytes are assembled into word writes of SAB/SAT rather than
 *     byte-wise memcpy into the peripheral registers.
 *
 * Cache note: no cache maintenance is needed around these DMA buffers. The
 * SAME5x bus matrix carries the CPU's system bus (master 0, "CM4S") and the
 * CMCC cache (master 1) as separate masters (datasheet Table 10-2), so SRAM
 * accesses do not pass through the cache; only flash does. That is also why
 * our NVMCTRL driver invalidates the CMCC after a flash write and nothing
 * else in this firmware does.
 */

/* ETHERNET_ENABLE comes from my_machine.h, which platformio.ini force-includes
   into every translation unit (-include src/my_machine.h). */
#if ETHERNET_ENABLE

#include <string.h>

#include "clearcore.h"
#include "gmac.h"

/* ------------------------------------------------------------------------ */
/* KSZ8081 PHY register map (Teknic Phy.h)                                   */
/* ------------------------------------------------------------------------ */

#define PHY_READ_OP         2
#define PHY_WRITE_OP        1

#define PHY_ADDRESS         0       /* strapped to 0 on the ClearCore */

#define PHY_B_CTRL          0x00    /* basic control    */
#define PHY_B_STATUS        0x01    /* basic status     */
#define PHY_ICS             0x1B    /* interrupt control/status */
#define PHY_CTRL_1          0x1E    /* PHY control 1 (negotiated mode) */

#define PHY_B_CTRL_RES      (1u << 15)  /* software reset */

#define PHY_B_STATUS_LU     (1u << 2)   /* link status (latching low) */

#define PHY_ICS_LU          (1u << 0)   /* link-up interrupt        */
#define PHY_ICS_RF          (1u << 1)   /* remote fault interrupt   */
#define PHY_ICS_LD          (1u << 2)   /* link-down interrupt      */
#define PHY_ICS_LUEN        (1u << 8)   /* link-up interrupt enable */
#define PHY_ICS_RFEN        (1u << 9)   /* remote fault int. enable */
#define PHY_ICS_LDEN        (1u << 10)  /* link-down int. enable    */

/* PHY_CTRL_1 operation mode indication, valid once auto-negotiation is done */
#define PHY_CTRL_AN_MSK     0x0007
#define PHY_CTRL_AN_SPD_MSK 0x0002  /* set = 100 Mbps  */
#define PHY_CTRL_AN_FD_MSK  0x0004  /* set = full duplex */

/*
 * The management interface clocks a fixed 32-bit frame, so NSR.IDLE always
 * comes back; the bound only protects against a GMAC that never got its
 * clocks. At 1.875 MHz MDC a transaction is ~17 us, i.e. ~2000 CPU cycles
 * per MDC bit at 120 MHz.
 */
#define PHY_MDIO_TIMEOUT    2000000u

/* ------------------------------------------------------------------------ */
/* Descriptor rings (Teknic EthernetApi.h sizes, kept verbatim)              */
/* ------------------------------------------------------------------------ */

#define RX_BUFF_CNT     16
#define RX_BUFFER_SIZE  128     /* must agree with GMAC->DCFGR.DRBS below */
#define TX_BUFF_CNT     8
#define TX_BUFFER_SIZE  520

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/* DCFGR.DRBS counts 64-byte units; we program 0x02. */
_Static_assert(RX_BUFFER_SIZE == 64 * 2, "RX_BUFFER_SIZE must match DCFGR.DRBS");
/* A maximum-length frame has to fit in the ring, or it can never be received. */
_Static_assert(RX_BUFF_CNT * RX_BUFFER_SIZE >= GMAC_MAX_FRAME_LEN, "rx ring too small");
_Static_assert(TX_BUFF_CNT * TX_BUFFER_SIZE >= GMAC_MAX_FRAME_LEN, "tx ring too small");
#endif

/* Receive buffer descriptor entry, datasheet Table 24-2. */
typedef union {
    struct {
        uint32_t OWN   : 1;  /* word0 b0     software owns the buffer      */
        uint32_t WRAP  : 1;  /* word0 b1     last descriptor in the list   */
        uint32_t ADDR  : 30; /* word0 b31:2  buffer address                */
        uint32_t LEN   : 13; /* word1 b12:0  frame length, FCS included    */
        uint32_t FCS   : 1;  /* word1 b13                                  */
        uint32_t SF    : 1;  /* word1 b14    start of frame                */
        uint32_t EF    : 1;  /* word1 b15    end of frame                  */
        uint32_t CFI   : 1;
        uint32_t VLAN  : 3;
        uint32_t PTAG  : 1;
        uint32_t VTAG  : 1;
        uint32_t CSM   : 2;
        uint32_t SNAP  : 1;
        uint32_t SPAMI : 2;
        uint32_t SPAM  : 1;
        uint32_t       : 1;
        uint32_t UHM   : 1;
        uint32_t MHM   : 1;
        uint32_t GAO   : 1;
    } bit;
    uint32_t reg[2];
} gmac_rx_desc_t;

/* Transmit buffer descriptor entry, datasheet Table 24-3. */
typedef union {
    struct {
        uint32_t ADDR  : 32; /* word0        buffer address                */
        uint32_t LEN   : 14; /* word1 b13:0  bytes in this buffer          */
        uint32_t       : 1;
        uint32_t LB    : 1;  /* word1 b15    last buffer of the frame      */
        uint32_t CRC   : 1;
        uint32_t       : 3;
        uint32_t CSER  : 3;
        uint32_t       : 3;
        uint32_t LCERR : 1;
        uint32_t FCERR : 1;
        uint32_t       : 1;
        uint32_t RLERR : 1;
        uint32_t WRAP  : 1;  /* word1 b30    last descriptor in the list   */
        uint32_t OWN   : 1;  /* word1 b31    software owns the buffer      */
    } bit;
    uint32_t reg[2];
} gmac_tx_desc_t;

static volatile gmac_rx_desc_t rx_desc[RX_BUFF_CNT] __attribute__((aligned(8)));
static volatile gmac_tx_desc_t tx_desc[TX_BUFF_CNT] __attribute__((aligned(8)));
static uint8_t rx_buffer[RX_BUFF_CNT][RX_BUFFER_SIZE] __attribute__((aligned(8)));
static uint8_t tx_buffer[TX_BUFF_CNT][TX_BUFFER_SIZE] __attribute__((aligned(8)));

static uint16_t rx_index;       /* next receive descriptor to inspect */
static uint16_t tx_head;        /* next free transmit descriptor      */
static uint16_t tx_tail;        /* oldest transmit descriptor in flight */
static uint16_t tx_pending;     /* transmit descriptors held by the GMAC */

static bool phy_init_failed;
static bool link_state;

static volatile uint32_t rx_event_count;
static volatile uint32_t tx_event_count;

#define RX_NEXT(i) (((i) + 1u) % RX_BUFF_CNT)
#define TX_NEXT(i) (((i) + 1u) % TX_BUFF_CNT)

/* ------------------------------------------------------------------------ */
/* MDIO                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Enable the GMAC management port and run one shift operation to the PHY. The
 * PHY register contents come back in the GMAC's MAN register.
 */
static uint32_t phy_shift (uint32_t phy_op, uint32_t phy_reg, uint32_t contents)
{
    uint32_t guard = PHY_MDIO_TIMEOUT;

    GMAC->NCR.bit.MPE = 1;
    GMAC->MAN.reg = GMAC_MAN_CLTTO |                /* clause 22 (1 is 22) */
                    GMAC_MAN_OP(phy_op) |           /* read or write       */
                    GMAC_MAN_PHYA(PHY_ADDRESS) |
                    GMAC_MAN_REGA(phy_reg) |
                    GMAC_MAN_WTN(0x2) |             /* must be written '1' '0' */
                    GMAC_MAN_DATA(contents);

    while (!(GMAC->NSR.reg & GMAC_NSR_IDLE)) {
        if (--guard == 0) {
            GMAC->NCR.bit.MPE = 0;
            return 0xFFFF;      /* reads as "no PHY" to the callers below */
        }
    }
    GMAC->NCR.bit.MPE = 0;

    return GMAC_MAN_DATA(GMAC->MAN.reg);
}

static uint32_t phy_read (uint32_t phy_reg)
{
    return phy_shift(PHY_READ_OP, phy_reg, 0);
}

static uint32_t phy_write (uint32_t phy_reg, uint32_t contents)
{
    phy_shift(PHY_WRITE_OP, phy_reg, contents);
    return phy_shift(PHY_READ_OP, phy_reg, 0);
}

/*
 * Software-reset the PHY and prove the management interface works. Ported
 * from EthernetManager::PhyInitialize.
 *
 * The interrupt-enable write is kept because it doubles as the readback test
 * that MDIO writes stick. Nothing listens to the resulting INTRP output: the
 * PHY interrupt pin (PC28 / EXTINT12) is deliberately left unclaimed.
 */
static void phy_initialize (void)
{
    uint32_t mask, value;

    phy_init_failed = false;
    link_state = false;

    /* Verify that the PHY is online. */
    if (phy_read(PHY_B_CTRL) == 0xFFFF) {
        phy_init_failed = true;
        return;
    }

    phy_write(PHY_B_CTRL, PHY_B_CTRL_RES);

    if (phy_read(PHY_ICS) != 0) {
        phy_init_failed = true;
        return;
    }

    mask = PHY_ICS_LDEN | PHY_ICS_LUEN | PHY_ICS_RFEN;
    value = phy_write(PHY_ICS, mask);

    /* Verify the interrupts were set correctly. Ignore the 8 LSB. */
    if ((mask >> 8) != (value >> 8))
        phy_init_failed = true;
}

/* ------------------------------------------------------------------------ */
/* MAC address                                                               */
/* ------------------------------------------------------------------------ */

/*
 * Teknic's factory MAC address lives in the 512-byte NVM user page. Their
 * NvmManager addresses that page in "locations" that skip the first 32 bytes
 * of automatic calibration data (NVM_LOCATION_TO_INDEX(loc) = loc + 32), so
 * NVM_LOC_MAC_FIRST (logical 468) is byte 500 of the page and the second word
 * is byte 504.
 */
#define NVM_LOC_USER_MAX    (NVMCTRL_PAGE_SIZE - 32)    /* 480 */
#define NVM_LOC_MAC_FIRST   (NVM_LOC_USER_MAX - 12)     /* 468 */
#define NVM_PAGE_INDEX(loc) ((loc) + 32)

/* NvmManager.cpp:59 — the address the board falls back to. */
#define DEFAULT_MAC_ADDRESS 0x241510b00000ULL

/* Ported from NvmManager::MacAddress + NvmManager::Int64. */
static void mac_address_read (uint8_t *mac)
{
    const uint8_t *page = (const uint8_t *)NVMCTRL_USER;
    uint32_t first, second;
    uint64_t mac_nvm;
    int8_t shift;

    /*
     * 64-bit reads don't work if they aren't aligned; read it as 2 32-bit
     * values instead. The first location holds the high half.
     */
    memcpy(&first, page + NVM_PAGE_INDEX(NVM_LOC_MAC_FIRST), sizeof(first));
    memcpy(&second, page + NVM_PAGE_INDEX(NVM_LOC_MAC_FIRST) + sizeof(first),
           sizeof(second));

    mac_nvm = ((uint64_t)first << 32) | second;

    /*
     * If an invalid MAC address is detected, revert to the default MAC
     * address to be able to come online.
     */
    if (mac_nvm == UINT64_MAX || (mac_nvm >> 48))
        mac_nvm = DEFAULT_MAC_ADDRESS;

    for (shift = 5; shift >= 0; shift--)
        mac[5 - shift] = (uint8_t)((mac_nvm >> (shift * 8)) & 0xFF);
}

/* ------------------------------------------------------------------------ */
/* Transmit / receive enable                                                 */
/* ------------------------------------------------------------------------ */

static void tx_ring_reset (void)
{
    uint16_t i;

    for (i = 0; i < TX_BUFF_CNT; i++) {
        tx_desc[i].bit.OWN = 1;     /* mark the entries as used       */
        tx_desc[i].bit.LB = 1;      /* mark the entries as individual */
    }
    tx_head = tx_tail = tx_pending = 0;
}

/*
 * Enable or disable transmit and receive of frames. GMAC configuration
 * registers must only be written while both are off.
 */
static void gmac_enable (bool enable)
{
    bool enabled = (GMAC->NCR.bit.TXEN && GMAC->NCR.bit.RXEN);

    if (enable == enabled)
        return;                 /* nothing to do */

    GMAC->NCR.bit.TXEN = enable ? 1 : 0;
    GMAC->NCR.bit.RXEN = enable ? 1 : 0;

    if (!enable) {
        /*
         * Writing TXEN to 0 on the GMAC resets the GMAC's transmit queue
         * pointer to the address of tx_buffer[0], so our transmit indices
         * have to go back to the start with it.
         *
         * The receive ring is deliberately left alone: "Disabling receive
         * does not have the same effect on the receive buffer queue pointer"
         * (datasheet 24.6.3.5), so the hardware resumes where it stopped and
         * rx_index must stay in step with it.
         */
        tx_ring_reset();
    }
}

/* ------------------------------------------------------------------------ */
/* Initialization                                                            */
/* ------------------------------------------------------------------------ */

void gmac_init (uint8_t mac_out[6])
{
    uint8_t mac[6];
    uint16_t i;

    /* Bus clocks. The GMAC has no generic clock: the RMII reference clock
       arrives from the PHY on GTXCK (PA14) and MDC divides down from MCK. */
    CLOCK_ENABLE(AHBMASK, GMAC_);
    CLOCK_ENABLE(APBCMASK, GMAC_);

    /* Disable transmit and receive circuits.
       Must be done prior to configuring GMAC settings. */
    gmac_enable(false);

    /* Write GMAC settings */
    GMAC->NCR.bit.MPE = 1;          /* Management port enabled */
    GMAC->NCFGR.bit.SPD = 1;        /* 100 Mbps */
    GMAC->NCFGR.bit.FD = 1;         /* Full duplex mode */
    GMAC->NCFGR.bit.MAXFS = 1;      /* Increase max frame size */
    GMAC->NCFGR.bit.CLK = 0x04;     /* MCK divided by 64 -> 1.875 MHz MDC */
    GMAC->UR.bit.MII = 0;           /* RMII mode */
    GMAC->DCFGR.bit.FBLDO = 0x04;   /* Use INCR4 AHB bursts */
    GMAC->DCFGR.bit.RXBMS = 0x03;   /* 4 Kbytes receiver packet buffer mem size */
    GMAC->DCFGR.bit.TXPBMS = 0x01;  /* 4 Kb transmitter packet buffer mem size */
    GMAC->DCFGR.bit.DRBS = 0x02;    /* 128 bytes receiver buffer in AHB */
    GMAC->WOL.reg = 0;
    GMAC->IPGS.reg = GMAC_IPGS_FL((0x1 << 8) | 0x1);

    /* Initialize Receive Descriptor List */
    for (i = 0; i < RX_BUFF_CNT; i++) {
        rx_desc[i].reg[0] = (uint32_t)&rx_buffer[i][0];  /* clears OWN + WRAP */
        rx_desc[i].reg[1] = 0;
    }
    /* Mark the last descriptor in the queue to wrap */
    rx_desc[RX_BUFF_CNT - 1].bit.WRAP = 1;
    rx_index = 0;

    /* Initialize Transmit Descriptor List */
    for (i = 0; i < TX_BUFF_CNT; i++) {
        tx_desc[i].reg[0] = (uint32_t)&tx_buffer[i][0];
        tx_desc[i].reg[1] = 0;
    }
    tx_ring_reset();
    /* Mark the last descriptor in the queue to wrap */
    tx_desc[TX_BUFF_CNT - 1].bit.WRAP = 1;

    /* Write address of transmit buffer descriptor list to register transmit
       buffer queue pointer.
       *Must be done while transmit is disabled. */
    GMAC->TBQB.reg = (uint32_t)&tx_desc[0];
    /* Write address of receive buffer descriptor list to register receive
       buffer queue pointer.
       *Must be done while receive is disabled. */
    GMAC->RBQB.reg = (uint32_t)&rx_desc[0];

    /* Reset interrupts. Priority 3 per PLAN.md §3 — below the step and pulse
       timers (0) and the EIC limit/control inputs (2). */
    NVIC_DisableIRQ(GMAC_IRQn);
    NVIC_ClearPendingIRQ(GMAC_IRQn);
    NVIC_SetPriority(GMAC_IRQn, 3);
    NVIC_EnableIRQ(GMAC_IRQn);

    /* Configure the GPIO pins. Every RMII and MDIO signal is peripheral
       function L (11) on this part. PMUXEN hands the pad to the peripheral
       outright, which is why no direction or input-enable setup is needed. */
    pin_pmux(GRP_A, 17, 11);    /* GTXEN */
    pin_pmux(GRP_A, 18, 11);    /* GTX0  */
    pin_pmux(GRP_A, 19, 11);    /* GTX1  */
    pin_pmux(GRP_A, 13, 11);    /* GRX0  */
    pin_pmux(GRP_A, 12, 11);    /* GRX1  */
    pin_pmux(GRP_A, 15, 11);    /* GRXER */
    pin_pmux(GRP_C, 20, 11);    /* GRXDV */
    pin_pmux(GRP_C, 12, 11);    /* GMDIO */
    pin_pmux(GRP_C, 11, 11);    /* GMDC  */
    pin_pmux(GRP_A, 14, 11);    /* GTXCK — RMII 50 MHz reference from the PHY */

    /* PC28 (PHY INTRP, EXTINT12) is intentionally not claimed. */

    /* Enable appropriate GMAC interrupts. */
    GMAC->IER.bit.TCOMP = 1;    /* Transmit complete */
    GMAC->IER.bit.RCOMP = 1;    /* Receive complete */

    /* Initialize the PHY */
    phy_initialize();

    /* Retrieve the MAC address from NVM and program the specific-address
       filter. SAB must be written before SAT: writing the top half is what
       activates the address match. */
    mac_address_read(mac);
    GMAC->Sa[0].SAB.reg = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                          ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    GMAC->Sa[0].SAT.reg = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);

    memcpy(mac_out, mac, sizeof(mac));

    gmac_enable(true);
}

bool gmac_phy_init_failed (void)
{
    return phy_init_failed;
}

uint32_t gmac_rx_events (void)
{
    return rx_event_count;
}

uint32_t gmac_tx_events (void)
{
    return tx_event_count;
}

/* ------------------------------------------------------------------------ */
/* Link status                                                               */
/* ------------------------------------------------------------------------ */

bool gmac_link_up (void)
{
    bool up;

    if (phy_init_failed)
        return false;

    /*
     * Clause 22 link status latches low: a link that dropped and came back
     * still reads 0 once, to make the failure observable. Read twice and
     * believe the second read.
     */
    (void)phy_read(PHY_B_STATUS);
    up = (phy_read(PHY_B_STATUS) & PHY_B_STATUS_LU) != 0;

    if (up && !link_state) {
        /*
         * Link-up edge. Match the GMAC to what auto-negotiation settled on
         * (ported from EthernetManager::IrqHandlerPhy, which did this from
         * the PHY interrupt).
         */
        uint16_t phy_mode;
        bool enabled = (GMAC->NCR.bit.TXEN == 1);

        /* Disable must be done before writing GMAC settings. */
        gmac_enable(false);

        phy_mode = (uint16_t)(phy_read(PHY_CTRL_1) & PHY_CTRL_AN_MSK);

        GMAC->NCFGR.bit.SPD = (phy_mode & PHY_CTRL_AN_SPD_MSK) ? 1 : 0;
        GMAC->NCFGR.bit.FD = (phy_mode & PHY_CTRL_AN_FD_MSK) ? 1 : 0;

        gmac_enable(enabled);
    }

    link_state = up;

    return up;
}

/* ------------------------------------------------------------------------ */
/* Receive                                                                   */
/* ------------------------------------------------------------------------ */

/* Hand one receive descriptor back to the GMAC and step past it. */
static void rx_release (void)
{
    rx_desc[rx_index].bit.OWN = 0;
    rx_index = RX_NEXT(rx_index);
}

uint16_t gmac_rx_frame (uint8_t *buf, uint16_t max_len)
{
    uint16_t attempt;

    /*
     * Each pass: drop leading descriptors that are not a start of frame,
     * then walk from the start of frame to the end of frame. A receive error
     * can leave a fragment in the ring — "software can detect this by looking
     * for start of frame bit set in a buffer following a buffer with no end
     * of frame bit set" (datasheet 24.6.3.3) — so a run with no end of frame
     * is released and the search restarts.
     */
    for (attempt = 0; attempt < RX_BUFF_CNT; attempt++) {

        uint16_t idx, nbuf = 0, i, copied = 0;
        uint32_t frame_len = 0, data_len;
        bool complete = false, incomplete = false;

        while (rx_desc[rx_index].bit.OWN && !rx_desc[rx_index].bit.SF)
            rx_release();

        if (!rx_desc[rx_index].bit.OWN)
            return 0;               /* ring holds nothing for us */

        idx = rx_index;

        for (i = 0; i < RX_BUFF_CNT; i++) {
            if (!rx_desc[idx].bit.OWN) {
                incomplete = true;  /* the DMA is still filling this frame */
                break;
            }
            if (i && rx_desc[idx].bit.SF)
                break;              /* a new frame starts: this run is a fragment */
            nbuf++;
            if (rx_desc[idx].bit.EF) {
                /* Only the final descriptor of a frame carries the length,
                   and it counts the FCS. */
                frame_len = rx_desc[idx].bit.LEN;
                complete = true;
                break;
            }
            idx = RX_NEXT(idx);
        }

        if (incomplete)
            return 0;

        if (!complete) {
            while (nbuf--)
                rx_release();
            continue;               /* fragment released, look again */
        }

        data_len = frame_len > 4 ? frame_len - 4 : 0;   /* strip the FCS */

        if (buf != NULL && data_len > 0 && data_len <= max_len) {
            uint32_t left = data_len;
            while (left) {
                uint32_t chunk = left < RX_BUFFER_SIZE ? left : RX_BUFFER_SIZE;
                memcpy(buf + copied,
                       (const void *)(rx_desc[rx_index].reg[0] & 0xFFFFFFFCu),
                       chunk);
                copied += chunk;
                left -= chunk;
                rx_release();
                nbuf--;
            }
        }

        /* Release whatever is left of the frame: the trailing FCS buffer, or
           all of it when the frame was dropped. */
        while (nbuf--)
            rx_release();

        if (copied)
            return (uint16_t)copied;

        /* Dropped (no destination, runt, or longer than the caller's buffer).
           Try the next frame. */
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Transmit                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Take back descriptors from completed transmissions.
 *
 * The GMAC only returns the first TX buffer descriptor to ownership on
 * transmission complete, so a frame is reclaimed as a unit: when its first
 * descriptor comes back, walk forward to the one we marked LB.
 */
static void tx_reclaim (void)
{
    while (tx_pending && tx_desc[tx_tail].bit.OWN) {
        bool last;
        do {
            last = tx_desc[tx_tail].bit.LB;
            tx_desc[tx_tail].bit.OWN = 1;
            tx_desc[tx_tail].bit.LB = 1;
            tx_tail = TX_NEXT(tx_tail);
            tx_pending--;
        } while (!last && tx_pending);
    }
}

bool gmac_tx_frame (const uint8_t *frame, uint16_t len)
{
    uint16_t needed, idx, i, left;

    if (frame == NULL || len == 0 || len > GMAC_MAX_FRAME_LEN)
        return false;

    tx_reclaim();

    needed = (len + TX_BUFFER_SIZE - 1) / TX_BUFFER_SIZE;
    if (needed > TX_BUFF_CNT - tx_pending)
        return false;

    idx = tx_head;
    left = len;

    for (i = 0; i < needed; i++) {
        uint16_t chunk = left > TX_BUFFER_SIZE ? TX_BUFFER_SIZE : left;

        memcpy(tx_buffer[idx], frame + (uint32_t)i * TX_BUFFER_SIZE, chunk);
        left -= chunk;

        /* Clear all fields except OWN and WRAP, then set LEN and LB. The
           descriptor stays software-owned until the whole frame is staged. */
        tx_desc[idx].reg[1] &= 0xC0000000u;
        tx_desc[idx].bit.LEN = chunk;
        tx_desc[idx].bit.LB = (left == 0);

        idx = TX_NEXT(idx);
    }

    /* Hand the buffers over back to front so the descriptor the GMAC polls —
       the first one — is released only once the rest of the frame is ready. */
    for (i = needed; i-- > 0; )
        tx_desc[(tx_head + i) % TX_BUFF_CNT].bit.OWN = 0;

    tx_head = idx;
    tx_pending += needed;

    __DMB();    /* descriptors visible before the DMA is told to look */

    /* Activate the transmit. */
    GMAC->NCR.bit.TSTART = 1;

    return true;
}

/* ------------------------------------------------------------------------ */
/* Interrupt                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Acknowledge only. Frames are moved in the foreground by gmac_rx_frame() and
 * gmac_tx_frame(); nothing here may grow, because at priority 3 it sits above
 * the housekeeping tick and would be one more thing between step pulses.
 *
 * TSR and RSR must be cleared or the interrupt re-asserts forever — they are
 * write-one-to-clear, so writing back exactly the bits we read leaves any
 * event that arrived in between pending.
 */
void GMAC_Handler (void)
{
    uint32_t tsr = GMAC->TSR.reg;   /* Transmit status register */
    uint32_t rsr = GMAC->RSR.reg;   /* Receive status register  */

    (void)GMAC->ISR.reg;            /* clear on read */

    if (tsr) {
        GMAC->TSR.reg = tsr;
        if (tsr & GMAC_TSR_TXCOMP)
            tx_event_count++;
    }

    if (rsr) {
        GMAC->RSR.reg = rsr;
        if (rsr & GMAC_RSR_REC)
            rx_event_count++;
    }
}

#endif /* ETHERNET_ENABLE */
