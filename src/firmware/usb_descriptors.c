// USB descriptors: one CDC ACM interface for the host link (F-R30)
#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"

// Raspberry Pi VID with the Pico SDK's CDC PID, for development
#define USB_VID 0x2E8A
#define USB_PID 0x000A

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // IAD, as required for CDC's two interfaces
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

// String indices match the descriptors above. The serial number is the
// flash unique ID, so /dev/serial/by-id/ gives each board a stable path.
static const char *const string_desc[] = {
    NULL,                   // 0: language, handled below
    "wurc",                 // 1: manufacturer
    "controller hub",       // 2: product
    NULL,                   // 3: serial number
    "Host link",            // 4: CDC interface
};

#define MAX_STRING_CHARS 32
static uint16_t desc_str[MAX_STRING_CHARS + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    size_t len;

    if (index == 0) {
        desc_str[1] = 0x0409;   // English (US)
        len = 1;
    } else {
        if (index >= sizeof string_desc / sizeof string_desc[0])
            return NULL;

        const char *str = string_desc[index];
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof serial);
            str = serial;
        }

        len = strlen(str);
        if (len > MAX_STRING_CHARS)
            len = MAX_STRING_CHARS;
        for (size_t i = 0; i < len; i++)
            desc_str[1 + i] = (uint8_t)str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
