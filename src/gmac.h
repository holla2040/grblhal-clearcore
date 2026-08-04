/*
 * gmac.h — bare-metal SAME5x GMAC (Ethernet MAC) + KSZ8081 RMII PHY driver
 * for grblhal-clearcore.
 *
 * Foreground-polled: the GMAC interrupt (NVIC priority 3, PLAN.md §3) only
 * acknowledges hardware status. Frames are moved by gmac_rx_frame() /
 * gmac_tx_frame() from the grblHAL foreground, never from an ISR.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 *
 * Register sequences adapted from Teknic ClearCore-library 1.7.4 (MIT) —
 * see the attribution block in gmac.c.
 */

#ifndef GMAC_H
#define GMAC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Longest frame we accept in either direction. GMAC->NCFGR.MAXFS is set, so
 * the receiver takes frames up to 1536 bytes; 1518 (1500 MTU + 14 header +
 * 4 FCS) is the largest untagged frame that can legally arrive.
 */
#define GMAC_MAX_FRAME_LEN 1536

/*
 * Bring up the GMAC and the PHY: bus clocks, RMII + MDIO pins, descriptor
 * rings, MAC address from the NVM user page, KSZ8081 software reset, GMAC
 * IRQ at priority 3, then transmit + receive enabled.
 *
 * mac_out receives the six MAC address bytes that were programmed into the
 * GMAC's specific-address filter, in wire order.
 */
void gmac_init(uint8_t mac_out[6]);

/*
 * Poll the PHY's link status over MDIO and, on a link-up edge, re-match the
 * GMAC's speed/duplex to what auto-negotiation settled on.
 *
 * Costs three MDIO transactions (~50 us at the 1.875 MHz MDC we configure),
 * so call it at a human timescale (a few times a second), not per frame.
 */
bool gmac_link_up(void);

/*
 * Copy one complete received frame into buf and release its descriptors.
 * Returns the frame length in bytes with the 4-byte FCS already stripped,
 * or 0 when the ring holds no complete frame.
 *
 * A frame longer than max_len is dropped rather than truncated. Passing
 * buf == NULL drops the next frame.
 */
uint16_t gmac_rx_frame(uint8_t *buf, uint16_t max_len);

/*
 * Copy a frame into the transmit ring and start transmission. Returns false
 * if len is out of range or the ring has no room, in which case nothing was
 * queued and the caller should retry later.
 */
bool gmac_tx_frame(const uint8_t *frame, uint16_t len);

/* True if the PHY did not answer over MDIO during gmac_init(). */
bool gmac_phy_init_failed(void);

/* GMAC interrupt event counters, for diagnostics only. */
uint32_t gmac_rx_events(void);
uint32_t gmac_tx_events(void);

#endif /* GMAC_H */
