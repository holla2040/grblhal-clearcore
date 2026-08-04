/*
 * tusb_config.h — TinyUSB configuration for grblhal-clearcore.
 * Device-only, CDC-ACM, no OS, SAME5x full speed.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

/* Our SAME53_DFP 2.0.11 names the USB IRQs USB_OTHER/SOF_HSOF/TRCPT0/TRCPT1;
   TinyUSB's samd port uses the newer packs' USB_0..USB_3 names — alias them.
   (This header is included first in every TinyUSB translation unit.) */
#define USB_0_IRQn USB_OTHER_IRQn
#define USB_1_IRQn USB_SOF_HSOF_IRQn
#define USB_2_IRQn USB_TRCPT0_IRQn
#define USB_3_IRQn USB_TRCPT1_IRQn

#define CFG_TUSB_MCU            OPT_MCU_SAME5X
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  512
#define CFG_TUD_CDC_TX_BUFSIZE  512
#define CFG_TUD_CDC_EP_BUFSIZE  64

#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#endif
