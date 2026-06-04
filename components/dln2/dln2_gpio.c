#include "dln2_private.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

#define DLN2_GPIO_CMD(cmd) DLN2_CMD(cmd, DLN2_MODULE_GPIO)

#define DLN2_GPIO_GET_PIN_COUNT       DLN2_GPIO_CMD(0x01)
#define DLN2_GPIO_SET_DEBOUNCE        DLN2_GPIO_CMD(0x04)
#define DLN2_GPIO_PORT_GET_VAL        DLN2_GPIO_CMD(0x06)
#define DLN2_GPIO_PIN_GET_VAL         DLN2_GPIO_CMD(0x0b)
#define DLN2_GPIO_PIN_SET_OUT_VAL     DLN2_GPIO_CMD(0x0c)
#define DLN2_GPIO_PIN_GET_OUT_VAL     DLN2_GPIO_CMD(0x0d)
#define DLN2_GPIO_PIN_ENABLE          DLN2_GPIO_CMD(0x10)
#define DLN2_GPIO_PIN_DISABLE         DLN2_GPIO_CMD(0x11)
#define DLN2_GPIO_PIN_SET_DIRECTION   DLN2_GPIO_CMD(0x13)
#define DLN2_GPIO_PIN_GET_DIRECTION   DLN2_GPIO_CMD(0x14)
#define DLN2_GPIO_PIN_SET_EVENT_CFG   DLN2_GPIO_CMD(0x1e)
#define DLN2_GPIO_PIN_GET_EVENT_CFG   DLN2_GPIO_CMD(0x1f)

#define DLN2_GPIO_DIRECTION_IN  0
#define DLN2_GPIO_DIRECTION_OUT 1

typedef struct {
    gpio_num_t gpio;
    bool enabled;
    bool is_output;
    bool out_value;
} dln2_gpio_line_t;

static const gpio_num_t s_gpio_map[] = {
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7,
    GPIO_NUM_8,
    GPIO_NUM_9,
};

#define DLN2_GPIO_COUNT ((uint16_t)(sizeof(s_gpio_map) / sizeof(s_gpio_map[0])))

static const char *TAG = "dln2_gpio";
static dln2_gpio_line_t s_lines[DLN2_GPIO_COUNT];

static bool get_line(uint16_t pin, dln2_gpio_line_t **line)
{
    if (pin >= DLN2_GPIO_COUNT) {
        return false;
    }

    *line = &s_lines[pin];
    return true;
}

static size_t response_pin_value(const dln2_request_t *request, uint16_t pin,
                                 const uint8_t *value, uint8_t *response,
                                 size_t response_cap)
{
    uint8_t payload[3] = { 0 };
    size_t payload_len = value ? sizeof(payload) : sizeof(uint16_t);

    dln2_put_le16(payload, pin);
    if (value) {
        payload[2] = *value ? 1 : 0;
    }

    return dln2_make_response(request, DLN2_RES_SUCCESS, payload, payload_len,
                              response, response_cap);
}

static uint16_t parse_pin_request(const dln2_request_t *request, bool has_value,
                                  uint16_t *pin, uint8_t *value)
{
    const size_t expected = has_value ? 3 : 2;

    if (request->payload_len != expected) {
        return DLN2_RES_INVALID_COMMAND_SIZE;
    }

    *pin = dln2_get_le16(request->payload);
    if (*pin >= DLN2_GPIO_COUNT) {
        return DLN2_RES_INVALID_PIN_NUMBER;
    }

    if (has_value) {
        *value = request->payload[2] ? 1 : 0;
    }

    return DLN2_RES_SUCCESS;
}

static size_t gpio_pin_enable(const dln2_request_t *request, bool enable,
                              uint8_t *response, size_t response_cap)
{
    dln2_gpio_line_t *line;
    uint16_t pin;
    uint8_t ignored;
    uint16_t result = parse_pin_request(request, false, &pin, &ignored);

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    get_line(pin, &line);

    if (enable) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << line->gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config GPIO%d failed: %s", line->gpio,
                     esp_err_to_name(err));
            return dln2_make_response(request, DLN2_RES_FAIL, NULL, 0,
                                      response, response_cap);
        }
        line->enabled = true;
        line->is_output = false;
        line->out_value = false;
    } else {
        gpio_reset_pin(line->gpio);
        line->enabled = false;
        line->is_output = false;
        line->out_value = false;
    }

    ESP_LOGI(TAG, "pin %u GPIO%d %s", pin, line->gpio,
             enable ? "enabled" : "disabled");

    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static uint16_t require_enabled_line(const dln2_request_t *request,
                                     bool has_value, uint16_t *pin,
                                     uint8_t *value, dln2_gpio_line_t **line)
{
    uint16_t result = parse_pin_request(request, has_value, pin, value);

    if (result != DLN2_RES_SUCCESS) {
        return result;
    }

    get_line(*pin, line);
    if (!(*line)->enabled) {
        return DLN2_RES_INVALID_PIN_NUMBER;
    }

    return DLN2_RES_SUCCESS;
}

static size_t gpio_get_value(const dln2_request_t *request, bool output_shadow,
                             uint8_t *response, size_t response_cap)
{
    dln2_gpio_line_t *line;
    uint16_t pin;
    uint8_t value = 0;
    uint16_t result = require_enabled_line(request, false, &pin, &value, &line);

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    value = output_shadow ? line->out_value : gpio_get_level(line->gpio);

    ESP_LOGI(TAG, "pin %u GPIO%d get%s=%u", pin, line->gpio,
             output_shadow ? "_out" : "", value);

    return response_pin_value(request, pin, &value, response, response_cap);
}

static size_t gpio_set_out_value(const dln2_request_t *request,
                                 uint8_t *response, size_t response_cap)
{
    dln2_gpio_line_t *line;
    uint16_t pin;
    uint8_t value = 0;
    uint16_t result = require_enabled_line(request, true, &pin, &value, &line);

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    esp_err_t err = gpio_set_level(line->gpio, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level GPIO%d failed: %s", line->gpio,
                 esp_err_to_name(err));
        return dln2_make_response(request, DLN2_RES_FAIL, NULL, 0, response,
                                  response_cap);
    }

    line->out_value = value;

    ESP_LOGI(TAG, "pin %u GPIO%d set_out=%u", pin, line->gpio, value);

    return response_pin_value(request, pin, NULL, response, response_cap);
}

static size_t dln2_gpio_set_direction(const dln2_request_t *request,
                                      uint8_t *response, size_t response_cap)
{
    dln2_gpio_line_t *line;
    gpio_mode_t mode;
    uint16_t pin;
    uint8_t value = 0;
    uint16_t result = require_enabled_line(request, true, &pin, &value, &line);

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    if (value != DLN2_GPIO_DIRECTION_IN && value != DLN2_GPIO_DIRECTION_OUT) {
        return dln2_make_response(request, DLN2_RES_INVALID_VALUE, NULL, 0,
                                  response, response_cap);
    }

    mode = value == DLN2_GPIO_DIRECTION_OUT ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    esp_err_t err = gpio_set_direction(line->gpio, mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_direction GPIO%d failed: %s", line->gpio,
                 esp_err_to_name(err));
        return dln2_make_response(request, DLN2_RES_FAIL, NULL, 0, response,
                                  response_cap);
    }

    line->is_output = value == DLN2_GPIO_DIRECTION_OUT;
    if (line->is_output) {
        gpio_set_level(line->gpio, line->out_value);
    }

    ESP_LOGI(TAG, "pin %u GPIO%d direction=%s", pin, line->gpio,
             line->is_output ? "out" : "in");

    return response_pin_value(request, pin, NULL, response, response_cap);
}

static size_t gpio_get_direction(const dln2_request_t *request,
                                 uint8_t *response, size_t response_cap)
{
    dln2_gpio_line_t *line;
    uint16_t pin;
    uint8_t value = 0;
    uint16_t result = require_enabled_line(request, false, &pin, &value, &line);

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    value = line->is_output ? DLN2_GPIO_DIRECTION_OUT : DLN2_GPIO_DIRECTION_IN;

    return response_pin_value(request, pin, &value, response, response_cap);
}

static size_t gpio_port_get_value(const dln2_request_t *request,
                                  uint8_t *response, size_t response_cap)
{
    uint8_t payload[4];
    uint32_t value = 0;

    if (request->payload_len != 0) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    for (uint16_t pin = 0; pin < DLN2_GPIO_COUNT; pin++) {
        if (s_lines[pin].enabled && gpio_get_level(s_lines[pin].gpio)) {
            value |= 1UL << pin;
        }
    }

    dln2_put_le32(payload, value);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(payload), response, response_cap);
}

void dln2_gpio_init(void)
{
    memset(s_lines, 0, sizeof(s_lines));

    for (uint16_t i = 0; i < DLN2_GPIO_COUNT; i++) {
        s_lines[i].gpio = s_gpio_map[i];
        gpio_reset_pin(s_lines[i].gpio);
    }

    ESP_LOGI(TAG, "initialized %u GPIO lines", DLN2_GPIO_COUNT);
}

size_t dln2_gpio_handle_request(const dln2_request_t *request,
                                uint8_t *response, size_t response_cap)
{
    uint8_t payload[2];

    switch (request->id) {
    case DLN2_GPIO_GET_PIN_COUNT:
        if (request->payload_len != 0) {
            return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                      NULL, 0, response, response_cap);
        }
        dln2_put_le16(payload, DLN2_GPIO_COUNT);
        return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                                  sizeof(payload), response, response_cap);

    case DLN2_GPIO_PIN_ENABLE:
        return gpio_pin_enable(request, true, response, response_cap);

    case DLN2_GPIO_PIN_DISABLE:
        return gpio_pin_enable(request, false, response, response_cap);

    case DLN2_GPIO_PIN_GET_VAL:
        return gpio_get_value(request, false, response, response_cap);

    case DLN2_GPIO_PIN_GET_OUT_VAL:
        return gpio_get_value(request, true, response, response_cap);

    case DLN2_GPIO_PIN_SET_OUT_VAL:
        return gpio_set_out_value(request, response, response_cap);

    case DLN2_GPIO_PIN_SET_DIRECTION:
        return dln2_gpio_set_direction(request, response, response_cap);

    case DLN2_GPIO_PIN_GET_DIRECTION:
        return gpio_get_direction(request, response, response_cap);

    case DLN2_GPIO_PORT_GET_VAL:
        return gpio_port_get_value(request, response, response_cap);

    case DLN2_GPIO_SET_DEBOUNCE:
    case DLN2_GPIO_PIN_SET_EVENT_CFG:
    case DLN2_GPIO_PIN_GET_EVENT_CFG:
    default:
        ESP_LOGW(TAG, "unsupported GPIO id=0x%04x", request->id);
        return dln2_make_response(request, DLN2_RES_NOT_IMPLEMENTED,
                                  NULL, 0, response, response_cap);
    }
}
