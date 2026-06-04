#include "dln2.h"

#include <string.h>

#include "dln2_private.h"
#include "esp_log.h"

static const char *TAG = "dln2";

void dln2_init(void)
{
    dln2_gpio_init();
}

size_t dln2_make_response(const dln2_request_t *request, uint16_t result,
                          const void *payload, size_t payload_len,
                          uint8_t *response, size_t response_cap)
{
    const size_t total_len = sizeof(dln2_response_t) + payload_len;

    if (!request || !response || total_len > response_cap) {
        return 0;
    }

    dln2_put_le16(response + 0, (uint16_t)total_len);
    dln2_put_le16(response + 2, request->id);
    dln2_put_le16(response + 4, request->echo);
    dln2_put_le16(response + 6, request->handle);
    dln2_put_le16(response + 8, result);

    if (payload_len && payload) {
        memcpy(response + sizeof(dln2_response_t), payload, payload_len);
    }

    ESP_LOGD(TAG, "TX id=0x%04x echo=%u handle=%u result=0x%02x payload=%u",
             request->id, request->echo, request->handle, result,
             (unsigned)payload_len);

    return total_len;
}

static size_t dln2_handle_ctrl(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap)
{
    uint8_t payload[4];

    switch (request->id) {
    case DLN2_CMD_GET_DEVICE_VER:
        if (request->payload_len != 0) {
            return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                      NULL, 0, response, response_cap);
        }
        dln2_put_le32(payload, DLN2_HW_ID);
        return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                                  sizeof(payload), response, response_cap);

    case DLN2_CMD_GET_DEVICE_SN:
        if (request->payload_len != 0) {
            return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                      NULL, 0, response, response_cap);
        }
        dln2_put_le32(payload, 0x12345678U);
        return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                                  sizeof(payload), response, response_cap);

    default:
        ESP_LOGW(TAG, "unsupported CTRL id=0x%04x", request->id);
        return dln2_make_response(request, DLN2_RES_NOT_IMPLEMENTED,
                                  NULL, 0, response, response_cap);
    }
}

size_t dln2_handle_request(const uint8_t *request_buf, size_t request_len,
                           uint8_t *response, size_t response_cap)
{
    dln2_request_t request = { 0 };
    uint16_t header_size;

    if (!request_buf || !response || request_len < sizeof(dln2_header_t)) {
        ESP_LOGW(TAG, "short request len=%u", (unsigned)request_len);
        return 0;
    }

    header_size = dln2_get_le16(request_buf);
    request.id = dln2_get_le16(request_buf + 2);
    request.echo = dln2_get_le16(request_buf + 4);
    request.handle = dln2_get_le16(request_buf + 6);

    ESP_LOGI(TAG, "RX size=%u len=%u id=0x%04x echo=%u handle=%u",
             header_size, (unsigned)request_len, request.id, request.echo,
             request.handle);

    if (header_size != request_len || header_size < sizeof(dln2_header_t)) {
        return dln2_make_response(&request, DLN2_RES_INVALID_MESSAGE_SIZE,
                                  NULL, 0, response, response_cap);
    }

    request.payload = request_buf + sizeof(dln2_header_t);
    request.payload_len = request_len - sizeof(dln2_header_t);

    switch (request.handle) {
    case DLN2_HANDLE_CTRL:
        return dln2_handle_ctrl(&request, response, response_cap);

    case DLN2_HANDLE_GPIO:
        return dln2_gpio_handle_request(&request, response, response_cap);

    case DLN2_HANDLE_I2C:
    case DLN2_HANDLE_SPI:
    case DLN2_HANDLE_ADC:
        ESP_LOGW(TAG, "unsupported handle=%u id=0x%04x", request.handle,
                 request.id);
        return dln2_make_response(&request, DLN2_RES_NOT_IMPLEMENTED,
                                  NULL, 0, response, response_cap);

    default:
        ESP_LOGW(TAG, "invalid handle=%u id=0x%04x", request.handle,
                 request.id);
        return dln2_make_response(&request, DLN2_RES_INVALID_HANDLE,
                                  NULL, 0, response, response_cap);
    }
}
