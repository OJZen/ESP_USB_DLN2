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
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xa257,
    .idProduct = 0x2013,
    .bcdDevice = 0x0100,
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
