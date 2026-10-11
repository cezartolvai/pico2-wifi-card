/* ---------------------------------------------------------------------------
 * tusb_config.h - TinyUSB device configuration: one CDC-NCM network class.
 * ------------------------------------------------------------------------ */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------- board / MCU ------------------------------- */
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT        0
#endif
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED     OPT_MODE_DEFAULT_SPEED
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined by the Pico SDK (OPT_MCU_RP2)
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_NONE
#endif
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       BOARD_TUD_MAX_SPEED

/* TinyUSB needs to know what role roothub 0 plays; without this, tusb_init()
 * with no arguments is a compile error. */
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | BOARD_TUD_RHPORT)
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#endif

/* ------------------------- CDC-NCM transfer blocks ---------------------- */
/* Both must be comfortably larger than the MTU so that two full frames fit
 * into one NTB.  See tinyusb/src/class/net/ncm.h for the tuning notes.      */
#ifndef CFG_TUD_NCM_IN_NTB_MAX_SIZE
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE   (2 * 1514 + 100)
#endif
#ifndef CFG_TUD_NCM_OUT_NTB_MAX_SIZE
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE  (2 * 1514 + 100)
#endif
#ifndef CFG_TUD_NCM_IN_NTB_N
#define CFG_TUD_NCM_IN_NTB_N          2
#endif
#ifndef CFG_TUD_NCM_OUT_NTB_N
#define CFG_TUD_NCM_OUT_NTB_N         2
#endif

/* ------------------------------ endpoints ------------------------------- */
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE  64
#endif

/* Largest Ethernet frame the class will carry (default 1514). */
#ifndef CFG_TUD_NET_MTU
#define CFG_TUD_NET_MTU         1514
#endif

/* ------------------------------- classes -------------------------------- */
/* Only one of the two network drivers may be enabled. */
#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_NCM             1

/* Two CDC-ACM functions next to the NCM interface: instance 0 is the management
 * console (/dev/ttyACM0), instance 1 the debug stream (/dev/ttyACM1).  They are
 * numbered by descriptor order, not by interface number - see usb_descriptors.c
 * for the interface and endpoint layout. */
#define CFG_TUD_CDC             2
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_AUDIO           0
#define CFG_TUD_VIDEO           0

/* Console traffic is line-oriented and slow compared to a bulk transfer, so the
 * TinyUSB default 64-byte endpoint and FIFO sizes are enough; the TX FIFO is
 * what absorbs a state dump written in one go. */
#ifndef CFG_TUD_CDC_EP_BUFSIZE
#define CFG_TUD_CDC_EP_BUFSIZE  64
#endif
#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE  128
#endif
#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE  512
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
