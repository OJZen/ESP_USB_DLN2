#include "usb_descriptors.h"

#include "tusb.h"

enum {
    ITF_NUM_DLN2 = 0,
    ITF_NUM_TOTAL,
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
};

#define DLN2_EP_OUT 0x01
#define DLN2_EP_IN  0x81

#define DLN2_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

const tusb_desc_device_t dln2_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xa257,
    .idProduct = 0x2013,
    .bcdDevice = 0x0101,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t dln2_fs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, DLN2_DESC_TOTAL_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_DLN2, STRID_INTERFACE, DLN2_EP_OUT,
                          DLN2_EP_IN, 64),
};

const char *dln2_string_descriptor[] = {
    (const char[]){0x09, 0x04},
    "Diolan",
    "DLN-2 USB GPIO",
    "12345678",
    "DLN2",
};

const int dln2_string_descriptor_count =
    sizeof(dln2_string_descriptor) / sizeof(dln2_string_descriptor[0]);

#define DLN2_MS_OS_20_LEN 162
#define DLN2_MS_VENDOR_CODE 0x20

static const uint8_t dln2_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(DLN2_MS_OS_20_LEN, DLN2_MS_VENDOR_CODE),
};

/* A single-interface device needs the WinUSB ID at device scope. */
static const uint8_t dln2_ms_os_20_descriptor[] = {
    U16_TO_U8S_LE(10), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(DLN2_MS_OS_20_LEN),
    U16_TO_U8S_LE(20), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    U16_TO_U8S_LE(132), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(7), U16_TO_U8S_LE(42),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0,
    'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0,
    'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(80),
    /* DeviceInterfaceGUIDs, shared with scripts/dln2_windows.py. */
    '{', 0, '3', 0, 'D', 0, '7', 0, 'F', 0, '8', 0, 'D', 0, '6', 0, 'E', 0,
    '-', 0, 'A', 0, '4', 0, '5', 0, '7', 0,
    '-', 0, '4', 0, 'D', 0, 'B', 0, '6', 0,
    '-', 0, '9', 0, 'C', 0, '0', 0, 'F', 0,
    '-', 0, '5', 0, 'B', 0, 'A', 0, '0', 0, 'E', 0, 'A', 0,
    '8', 0, 'D', 0, '7', 0, '4', 0, 'C', 0, '1', 0,
    '}', 0, 0, 0, 0, 0,
};

_Static_assert(sizeof(dln2_ms_os_20_descriptor) == DLN2_MS_OS_20_LEN,
               "Incorrect Microsoft OS 2.0 descriptor length");

const uint8_t *tud_descriptor_bos_cb(void)
{
    return dln2_bos_descriptor;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bmRequestType != 0xc0 ||
        request->bRequest != DLN2_MS_VENDOR_CODE ||
        request->wIndex != 7 || request->wValue != 0) {
        return false;
    }

    return tud_control_xfer(rhport, request,
                            (void *)(uintptr_t)dln2_ms_os_20_descriptor,
                            sizeof(dln2_ms_os_20_descriptor));
}
