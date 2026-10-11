/* ---------------------------------------------------------------------------
 * usb_descriptors.c - a single CDC-NCM network interface.
 *
 * bcdUSB is 0x0201 (USB 2.1) and a BOS + Microsoft OS 2.0 descriptor set is
 * exposed so that Windows 10 picks up its built-in WinNCM driver without any
 * manual driver installation.
 * ------------------------------------------------------------------------ */
#include "tusb.h"

#include "pico/unique_id.h"

/* tud_network_mac_address[] is declared by TinyUSB's net_device.h. */

/* Same VID/PID layout as the TinyUSB examples: the interface combination has
 * to be part of the PID or the host will reuse a stale driver binding. */
#define _PID_MAP(itf, n)  ((CFG_TUD_##itf) << (n))
#define USB_PID           (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | \
                           _PID_MAP(MIDI, 3) | _PID_MAP(VENDOR, 4) | _PID_MAP(ECM_RNDIS, 5) | _PID_MAP(NCM, 5))

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_MAC,
    STRID_INTERFACE_MGMT,
    STRID_INTERFACE_DEBUG
};

/* TinyUSB opens interfaces in descriptor order and numbers the CDC-ACM
 * instances as it goes, so the function declared first here is instance 0
 * (/dev/ttyACM0) and the next one is instance 1. */
enum {
    ITF_NUM_CDC = 0,          /* CDC-NCM control                          */
    ITF_NUM_CDC_DATA,
    ITF_NUM_CDC_MGMT,             /* CDC-ACM instance 0: /dev/ttyACM0     */
    ITF_NUM_CDC_MGMT_DATA,
    ITF_NUM_CDC_DEBUG,            /* CDC-ACM instance 1: /dev/ttyACM1     */
    ITF_NUM_CDC_DEBUG_DATA,
    ITF_NUM_TOTAL
};

enum {
    CONFIG_ID_NCM = 0,
    CONFIG_ID_COUNT
};

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0201,

    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x2E8A,   /* Raspberry Pi Trading VID */
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,

    .bNumConfigurations = CONFIG_ID_COUNT
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN + \
                           2 * TUD_CDC_DESC_LEN)

/* RP2350 has 16 endpoints; this device uses 10 of them: EP0, three for
 * CDC-NCM and three for each of the two CDC-ACM functions. */
#define EPNUM_NET_NOTIF   0x81
#define EPNUM_NET_OUT     0x02
#define EPNUM_NET_IN      0x82

/* Each CDC-ACM function needs three endpoints of its own: an interrupt IN for
 * the line-state notification, a bulk OUT for typed input and a bulk IN for the
 * state dumps.  They cannot share an address with each other or with NCM. */
#define EPNUM_MGMT_NOTIF  0x83
#define EPNUM_MGMT_OUT    0x03
#define EPNUM_MGMT_IN     0x84

#define EPNUM_DEBUG_NOTIF 0x85
#define EPNUM_DEBUG_OUT   0x04
#define EPNUM_DEBUG_IN    0x86

static uint8_t const ncm_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(CONFIG_ID_NCM + 1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),

    // Interface, description string, MAC string, notification EP, data EPs (out, in), size, max segment size
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_CDC, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64,
                           EPNUM_NET_OUT, EPNUM_NET_IN, CFG_TUD_NET_ENDPOINT_SIZE, CFG_TUD_NET_MTU),

    // Management console: interface, string, notification EP (+size), data EPs (out, in), EP size
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_MGMT, STRID_INTERFACE_MGMT, EPNUM_MGMT_NOTIF, 16,
                       EPNUM_MGMT_OUT, EPNUM_MGMT_IN, CFG_TUD_CDC_EP_BUFSIZE),

    // Debug stream: same shape, write-only in practice
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_DEBUG, STRID_INTERFACE_DEBUG, EPNUM_DEBUG_NOTIF, 16,
                       EPNUM_DEBUG_OUT, EPNUM_DEBUG_IN, CFG_TUD_CDC_EP_BUFSIZE),
};

/* A descriptor whose declared length does not match its contents is a bus error
 * waiting to happen, so make the compiler check it. */
TU_VERIFY_STATIC(sizeof(ncm_configuration) == CONFIG_TOTAL_LEN,
                 "configuration length does not match its descriptors");

TU_VERIFY_STATIC(sizeof(ncm_configuration) == CONFIG_TOTAL_LEN,
                   "config descriptor length does not match its contents");

static uint8_t const *const configuration_arr[CONFIG_ID_COUNT] = {
    [CONFIG_ID_NCM] = ncm_configuration,
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    return (index < CONFIG_ID_COUNT) ? configuration_arr[index] : NULL;
}

//--------------------------------------------------------------------+
// BOS + Microsoft OS 2.0 Descriptors (Windows 10 NCM driver binding)
//--------------------------------------------------------------------+
#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN  0xB2

uint8_t const desc_bos[] = {
    // total length, number of device caps
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    // Microsoft OS 2.0 descriptor
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1),
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

uint8_t const desc_ms_os_20[] = {
    // Set header: length, type, windows version, total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header: length, type, configuration index, reserved, configuration total length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function Subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_CDC, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'N', 'C', 'M', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // MS OS 2.0 Registry property descriptor: length, type
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength, "DeviceInterfaceGUIDs\0" in UTF-16
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    // bPropertyData: {12345678-0D08-43FD-8B3E-127CA8AFFF9D}
    '{', 0x00, '1', 0x00, '2', 0x00, '3', 0x00, '4', 0x00, '5', 0x00, '6', 0x00, '7', 0x00, '8', 0x00, '-', 0x00,
    '0', 0x00, 'D', 0x00, '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00, '3', 0x00, 'F', 0x00, 'D', 0x00, '-', 0x00,
    '8', 0x00, 'B', 0x00, '3', 0x00, 'E', 0x00, '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00,
    '8', 0x00, 'A', 0x00, 'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
static char const *const string_desc_arr[] = {
    [STRID_LANGID]           = (const char[]){0x09, 0x04},  // English (0x0409)
    [STRID_MANUFACTURER]     = "Raspberry Pi",
    [STRID_PRODUCT]          = "Pico 2 W Wi-Fi Card",
    [STRID_SERIAL]           = NULL,                        // filled from the board unique id
    [STRID_INTERFACE]        = "Pico 2 Wi-Fi (CDC-NCM)",
    /* STRID_MAC is handled separately */
    [STRID_INTERFACE_MGMT]   = "Pico 2 Wi-Fi management (ttyACM0)",
    [STRID_INTERFACE_DEBUG]  = "Pico 2 Wi-Fi debug (ttyACM1)",
};

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    unsigned chr_count = 0;

    switch (index) {
        case STRID_LANGID:
            memcpy(&desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        case STRID_SERIAL: {
            static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
            pico_unique_board_id_t id;
            pico_get_unique_board_id(&id);
            for (size_t i = 0; i < sizeof(id.id); i++) {
                snprintf(&serial[2 * i], 3, "%02X", id.id[i]);
            }
            chr_count = sizeof(serial) - 1;
            for (size_t i = 0; i < chr_count; i++) {
                desc_str[1 + i] = serial[i];
            }
            break;
        }

        case STRID_MAC:
            /* CDC NCM wants iMACAddress as 12 upper case hex digits */
            for (size_t i = 0; i < sizeof(tud_network_mac_address); i++) {
                desc_str[1 + chr_count++] = "0123456789ABCDEF"[(tud_network_mac_address[i] >> 4) & 0xf];
                desc_str[1 + chr_count++] = "0123456789ABCDEF"[(tud_network_mac_address[i] >> 0) & 0xf];
            }
            break;

        default: {
            const char *str = (index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
                              ? string_desc_arr[index] : NULL;
            if (str == NULL) {
                return NULL;
            }
            chr_count = (unsigned)strlen(str);
            size_t const max_count = sizeof(desc_str) / sizeof(desc_str[0]) - 1;
            if (chr_count > max_count) {
                chr_count = (unsigned)max_count;
            }
            for (size_t i = 0; i < chr_count; i++) {
                desc_str[1 + i] = str[i];
            }
            break;
        }
    }

    /* first byte is length (including header), second byte is string type */
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return desc_str;
}
