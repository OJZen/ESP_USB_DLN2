#include <stdbool.h>
#include <stdint.h>

#include "dln2.h"
#include "esp_err.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const char *TAG = "esp_usb_dln2";

static uint8_t s_rx_buf[DLN2_MAX_PACKET_SIZE];
static uint8_t s_tx_buf[DLN2_MAX_PACKET_SIZE];

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    uint32_t available = tud_vendor_n_available(itf);
    if (available > sizeof(s_rx_buf)) {
        ESP_LOGW(TAG, "dropping oversized vendor RX: %u bytes",
                 (unsigned)available);
        tud_vendor_n_read_flush(itf);
        return;
    }

    uint32_t read = tud_vendor_n_read(itf, s_rx_buf, available);
    if (read == 0) {
        return;
    }

    size_t response_len = dln2_handle_request(s_rx_buf, read, s_tx_buf,
                                              sizeof(s_tx_buf));
    if (response_len == 0) {
        return;
    }

    uint32_t written = tud_vendor_n_write(itf, s_tx_buf, response_len);
    tud_vendor_n_write_flush(itf);

    if (written != response_len) {
        ESP_LOGW(TAG, "short vendor TX: wrote %u of %u",
                 (unsigned)written, (unsigned)response_len);
    }
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;

    return stage != CONTROL_STAGE_SETUP;
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting ESP32-S3 DLN-2 USB GPIO device");

    dln2_init();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &dln2_device_descriptor;
    tusb_cfg.descriptor.string = dln2_string_descriptor;
    tusb_cfg.descriptor.string_count = dln2_string_descriptor_count;
    tusb_cfg.descriptor.full_speed_config = dln2_fs_configuration_descriptor;

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_LOGI(TAG, "TinyUSB installed; waiting for Linux dln2 driver");
}
