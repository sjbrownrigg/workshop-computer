#include "tusb.h"
#include <pico/unique_id.h>

// v2 PID differs from v1 (0x10C1) so OSes don't cache the old single-MIDI
// descriptor and confuse it with the new composite CDC+MIDI one.
#define USB_VID 0x2E8A  // Raspberry Pi
#define USB_PID 0x10C2  // StepBridge v2 composite
#define USB_BCD 0x0200

// ── String descriptors ──────────────────────────────────────────────────────

enum {
    STRING_LANGID = 0,
    STRING_MANUFACTURER,
    STRING_PRODUCT,
    STRING_SERIAL,
    STRING_CDC,      // shown as the CDC interface name in the OS serial port list
    STRING_LAST,
};

static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 }, // 0: English
    "Music Thing",                 // 1: Manufacturer
    "StepBridge 2",                // 2: Product (shown in OS device manager)
    NULL,                          // 3: Serial (generated from RP2040 flash ID)
    "StepBridge Control",          // 4: CDC interface label (shown in serial port picker)
};

// ── Device descriptor ───────────────────────────────────────────────────────
// bDeviceClass 0xEF / SubClass 0x02 / Protocol 0x01 = IAD (Interface
// Association Descriptor). Required whenever a single USB device contains
// multiple interface classes (here: CDC + Audio/MIDI). Without IAD the host
// cannot reliably associate the two CDC interfaces with each other.

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0xEF,  // Misc — required for IAD
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,  // IAD
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0200,
    .iManufacturer      = STRING_MANUFACTURER,
    .iProduct           = STRING_PRODUCT,
    .iSerialNumber      = STRING_SERIAL,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// ── Configuration descriptor ────────────────────────────────────────────────
// 4 interfaces total:
//   0: CDC Control  (notification endpoint)
//   1: CDC Data     (bulk IN + OUT)
//   2: MIDI Audio Control (required by USB Audio class structure, zero-length)
//   3: MIDI Streaming (bulk IN + OUT)

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL,
};

// Endpoint addresses. The RP2040 USB block has 16 endpoints (EP0–EP15,
// each bidirectional). Convention: IN endpoints have the 0x80 bit set.
#define EPNUM_CDC_NOTIF  0x81  // EP1 IN  — CDC notification (interrupt)
#define EPNUM_CDC_OUT    0x02  // EP2 OUT — CDC data host→device
#define EPNUM_CDC_IN     0x82  // EP2 IN  — CDC data device→host
#define EPNUM_MIDI_OUT   0x03  // EP3 OUT — MIDI host→device
#define EPNUM_MIDI_IN    0x83  // EP3 IN  — MIDI device→host

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    // bConfigurationValue=1, iConfiguration=0, bmAttributes=bus-powered, bMaxPower=100mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // CDC: ITF 0+1, string=STRING_CDC, notify EP 0x81 (8B packet), data EPs 0x02/0x82 (64B)
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRING_CDC,
                       EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    // MIDI: ITF 2+3, no string, EPs 0x03 out / 0x83 in (64B)
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

// ── String descriptor handler ────────────────────────────────────────────────

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == STRING_SERIAL) {
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        uint64_t uid = *(uint64_t *)&id.id;
        int serial = (int)((uid + 1) % 10000000ull);
        if (serial < 1000000) serial += 1000000;
        char tmp[16];
        chr_count = (uint8_t)sprintf(tmp, "%07d", serial);
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = tmp[i];
    } else if (index < STRING_LAST) {
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    } else {
        return NULL;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
