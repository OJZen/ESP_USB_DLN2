#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dln2.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const char *TAG = "esp_usb_dln2";

#define DLN2_VENDOR_ITF 0
#define DLN2_EVENT_WAIT_MS 50
#define DLN2_EVENT_USB_BACKOFF_MS 1
#define DLN2_HEADER_SIZE 8

static uint8_t s_rx_buf[DLN2_MAX_PACKET_SIZE];
static uint8_t s_rx_frame[DLN2_MAX_PACKET_SIZE];
static uint8_t s_tx_buf[DLN2_MAX_PACKET_SIZE];
static size_t s_rx_frame_len;
static size_t s_rx_frame_expected;
static SemaphoreHandle_t s_vendor_tx_lock;

static uint32_t dln2_vendor_write_packet(uint8_t itf, const uint8_t *packet,
                                         size_t packet_len)
{
    uint32_t written;

    if (s_vendor_tx_lock) {
        xSemaphoreTake(s_vendor_tx_lock, portMAX_DELAY);
    }

    written = tud_vendor_n_write(itf, packet, packet_len);
    tud_vendor_n_write_flush(itf);

    if (s_vendor_tx_lock) {
        xSemaphoreGive(s_vendor_tx_lock);
    }

    return written;
}

static uint16_t dln2_rx_frame_size(const uint8_t *frame)
{
    return (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
}

static void dln2_rx_frame_reset(void)
{
    s_rx_frame_len = 0;
    s_rx_frame_expected = 0;
}

static void dln2_rx_handle_frame(uint8_t itf)
{
    size_t response_len = dln2_handle_request(s_rx_frame, s_rx_frame_expected,
                                              s_tx_buf, sizeof(s_tx_buf));

    if (response_len > 0) {
        uint32_t written = dln2_vendor_write_packet(itf, s_tx_buf,
                                                    response_len);

        if (written != response_len) {
            ESP_LOGW(TAG, "short vendor TX: wrote %u of %u",
                     (unsigned)written, (unsigned)response_len);
        }
    }

    dln2_rx_frame_reset();
}

static void dln2_rx_feed(uint8_t itf, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        size_t target_len = s_rx_frame_expected > 0 ? s_rx_frame_expected :
                            DLN2_HEADER_SIZE;
        size_t copy_len = target_len - s_rx_frame_len;

        if (copy_len > len - offset) {
            copy_len = len - offset;
        }

        memcpy(s_rx_frame + s_rx_frame_len, data + offset, copy_len);
        s_rx_frame_len += copy_len;
        offset += copy_len;

        if (s_rx_frame_expected == 0 && s_rx_frame_len == DLN2_HEADER_SIZE) {
            s_rx_frame_expected = dln2_rx_frame_size(s_rx_frame);
            if (s_rx_frame_expected < DLN2_HEADER_SIZE ||
                s_rx_frame_expected > sizeof(s_rx_frame)) {
                ESP_LOGW(TAG, "dropping invalid DLN2 frame size=%u",
                         (unsigned)s_rx_frame_expected);
                dln2_rx_frame_reset();
            }
        }

        if (s_rx_frame_expected > 0 &&
            s_rx_frame_len == s_rx_frame_expected) {
            dln2_rx_handle_frame(itf);
        }
    }
}

static void dln2_event_task(void *arg)
{
    uint8_t event_buf[DLN2_MAX_PACKET_SIZE];

    (void)arg;

    while (true) {
        size_t event_len = 0;

        if (!tud_mounted()) {
            vTaskDelay(pdMS_TO_TICKS(DLN2_EVENT_WAIT_MS));
            continue;
        }

        if (tud_vendor_n_write_available(DLN2_VENDOR_ITF) < DLN2_HEADER_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(DLN2_EVENT_USB_BACKOFF_MS));
            continue;
        }

        if (!dln2_next_event(event_buf, sizeof(event_buf), &event_len,
                             DLN2_EVENT_WAIT_MS)) {
            continue;
        }

        do {
            uint32_t written = dln2_vendor_write_packet(DLN2_VENDOR_ITF,
                                                        event_buf, event_len);

            if (written != event_len) {
                ESP_LOGW(TAG, "short event TX: wrote %u of %u",
                         (unsigned)written, (unsigned)event_len);
                break;
            }
        } while (tud_vendor_n_write_available(DLN2_VENDOR_ITF) >=
                     DLN2_HEADER_SIZE &&
                 dln2_next_event(event_buf, sizeof(event_buf), &event_len, 0));
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    while (true) {
        uint32_t available = tud_vendor_n_available(itf);
        if (available == 0) {
            return;
        }

        uint32_t to_read = available;
        if (to_read > sizeof(s_rx_buf)) {
            to_read = sizeof(s_rx_buf);
        }

        uint32_t read = tud_vendor_n_read(itf, s_rx_buf, to_read);
        if (read == 0) {
            return;
        }

        dln2_rx_feed(itf, s_rx_buf, read);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting ESP32-S3 DLN-2 USB GPIO device");

    s_vendor_tx_lock = xSemaphoreCreateMutex();
    if (!s_vendor_tx_lock) {
        ESP_LOGE(TAG, "failed to create vendor TX mutex");
        return;
    }

    dln2_init();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &dln2_device_descriptor;
    tusb_cfg.descriptor.string = dln2_string_descriptor;
    tusb_cfg.descriptor.string_count = dln2_string_descriptor_count;
    tusb_cfg.descriptor.full_speed_config = dln2_fs_configuration_descriptor;

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    xTaskCreate(dln2_event_task, "dln2_event", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "TinyUSB installed; waiting for DLN-2 host");
}
