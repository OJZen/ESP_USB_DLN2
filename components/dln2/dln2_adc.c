#include "dln2_private.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define DLN2_ADC_CMD(cmd) DLN2_CMD(cmd, DLN2_MODULE_ADC)

#define DLN2_ADC_GET_CHANNEL_COUNT DLN2_ADC_CMD(0x01)
#define DLN2_ADC_ENABLE            DLN2_ADC_CMD(0x02)
#define DLN2_ADC_DISABLE           DLN2_ADC_CMD(0x03)
#define DLN2_ADC_CHANNEL_ENABLE    DLN2_ADC_CMD(0x05)
#define DLN2_ADC_CHANNEL_DISABLE   DLN2_ADC_CMD(0x06)
#define DLN2_ADC_SET_RESOLUTION    DLN2_ADC_CMD(0x08)
#define DLN2_ADC_CHANNEL_GET_VAL   DLN2_ADC_CMD(0x0a)
#define DLN2_ADC_CHANNEL_GET_ALL_VAL DLN2_ADC_CMD(0x0b)

#define DLN2_ADC_PORT 0
#define DLN2_ADC_RESOLUTION_BITS 10
#define DLN2_ADC_RAW_MAX_10BIT 1023
#define DLN2_ADC_ALL_VALUES_MAX_CHANNELS 8

typedef struct {
    gpio_num_t gpio;
    adc_channel_t channel;
} dln2_adc_channel_t;

typedef struct {
    bool unit_ready;
    bool port_enabled;
    uint8_t resolution_bits;
    adc_oneshot_unit_handle_t unit;
} dln2_adc_state_t;

static const dln2_adc_channel_t s_adc_map[] = {
    { .gpio = GPIO_NUM_14, .channel = ADC_CHANNEL_3 },
    { .gpio = GPIO_NUM_15, .channel = ADC_CHANNEL_4 },
    { .gpio = GPIO_NUM_16, .channel = ADC_CHANNEL_5 },
    { .gpio = GPIO_NUM_17, .channel = ADC_CHANNEL_6 },
};

#define DLN2_ADC_CHANNEL_COUNT \
    ((uint8_t)(sizeof(s_adc_map) / sizeof(s_adc_map[0])))

static const char *TAG = "dln2_adc";
static dln2_adc_state_t s_adc;
static bool s_adc_channel_enabled[DLN2_ADC_CHANNEL_COUNT];

static uint16_t adc_parse_port(const dln2_request_t *request, uint8_t *port)
{
    if (request->payload_len != sizeof(*port)) {
        return DLN2_RES_INVALID_COMMAND_SIZE;
    }

    *port = request->payload[0];
    if (*port != DLN2_ADC_PORT) {
        return DLN2_RES_INVALID_PORT_NUMBER;
    }

    return DLN2_RES_SUCCESS;
}

static uint16_t adc_parse_channel(const dln2_request_t *request, uint8_t *port,
                                  uint8_t *channel)
{
    if (request->payload_len != 2) {
        return DLN2_RES_INVALID_COMMAND_SIZE;
    }

    *port = request->payload[0];
    *channel = request->payload[1];

    if (*port != DLN2_ADC_PORT) {
        return DLN2_RES_INVALID_PORT_NUMBER;
    }

    if (*channel >= DLN2_ADC_CHANNEL_COUNT) {
        return DLN2_RES_INVALID_CHANNEL_NUMBER;
    }

    return DLN2_RES_SUCCESS;
}

static uint16_t adc_ensure_unit(void)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_2,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err;

    if (s_adc.unit_ready) {
        return DLN2_RES_SUCCESS;
    }

    err = adc_oneshot_new_unit(&unit_config, &s_adc.unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s",
                 esp_err_to_name(err));
        return DLN2_RES_FAIL;
    }

    for (uint8_t i = 0; i < DLN2_ADC_CHANNEL_COUNT; i++) {
        err = adc_oneshot_config_channel(s_adc.unit, s_adc_map[i].channel,
                                         &channel_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "config ch%u GPIO%d failed: %s", i,
                     s_adc_map[i].gpio, esp_err_to_name(err));
            adc_oneshot_del_unit(s_adc.unit);
            s_adc.unit = NULL;
            return DLN2_RES_FAIL;
        }
    }

    s_adc.unit_ready = true;
    ESP_LOGI(TAG, "ADC unit ready on ADC2 GPIO14-GPIO17");
    return DLN2_RES_SUCCESS;
}

static size_t adc_conflict_response(const dln2_request_t *request,
                                    uint16_t result, uint16_t conflict_mask,
                                    uint8_t *response, size_t response_cap)
{
    uint8_t payload[sizeof(uint16_t)];

    dln2_put_le16(payload, conflict_mask);
    return dln2_make_response(request, result, payload, sizeof(payload),
                              response, response_cap);
}

static size_t adc_get_channel_count(const dln2_request_t *request,
                                    uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint8_t payload = DLN2_ADC_CHANNEL_COUNT;
    uint16_t result = adc_parse_port(request, &port);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    return dln2_make_response(request, DLN2_RES_SUCCESS, &payload,
                              sizeof(payload), response, response_cap);
}

static size_t adc_set_resolution(const dln2_request_t *request,
                                 uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint8_t resolution;

    if (request->payload_len != 2) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    resolution = request->payload[1];
    if (port != DLN2_ADC_PORT) {
        return dln2_make_response(request, DLN2_RES_INVALID_PORT_NUMBER,
                                  NULL, 0, response, response_cap);
    }

    if (resolution != DLN2_ADC_RESOLUTION_BITS) {
        return dln2_make_response(request, DLN2_RES_INVALID_VALUE, NULL, 0,
                                  response, response_cap);
    }

    s_adc.resolution_bits = resolution;
    ESP_LOGI(TAG, "set ADC resolution=%u bits", resolution);
    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static size_t adc_channel_enable_request(const dln2_request_t *request,
                                         bool enable, uint8_t *response,
                                         size_t response_cap)
{
    uint8_t port;
    uint8_t channel;
    uint16_t result = adc_parse_channel(request, &port, &channel);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    if (enable) {
        result = adc_ensure_unit();
        if (result != DLN2_RES_SUCCESS) {
            return dln2_make_response(request, result, NULL, 0, response,
                                      response_cap);
        }

        result = dln2_pin_claim(s_adc_map[channel].gpio, DLN2_MODULE_ADC);
        if (result != DLN2_RES_SUCCESS) {
            return dln2_make_response(request, result, NULL, 0, response,
                                      response_cap);
        }

        s_adc_channel_enabled[channel] = true;
    } else {
        s_adc_channel_enabled[channel] = false;
        gpio_reset_pin(s_adc_map[channel].gpio);
        result = dln2_pin_release(s_adc_map[channel].gpio, DLN2_MODULE_ADC);
        if (result != DLN2_RES_SUCCESS) {
            return dln2_make_response(request, result, NULL, 0, response,
                                      response_cap);
        }
    }

    ESP_LOGI(TAG, "%s ADC ch%u GPIO%d",
             enable ? "enabled" : "disabled", channel,
             s_adc_map[channel].gpio);
    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static size_t adc_enable_request(const dln2_request_t *request, bool enable,
                                 uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint16_t result = adc_parse_port(request, &port);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    if (enable) {
        result = adc_ensure_unit();
        if (result != DLN2_RES_SUCCESS) {
            return adc_conflict_response(request, result, 0, response,
                                         response_cap);
        }
    }

    s_adc.port_enabled = enable;
    ESP_LOGI(TAG, "%s ADC port %u", enable ? "enabled" : "disabled",
             DLN2_ADC_PORT);
    return adc_conflict_response(request, DLN2_RES_SUCCESS, 0, response,
                                 response_cap);
}

static uint16_t adc_read_channel(uint8_t channel, uint16_t *value)
{
    int raw = 0;
    esp_err_t err;

    if (!s_adc.port_enabled || !s_adc_channel_enabled[channel] ||
        !dln2_pin_is_claimed(s_adc_map[channel].gpio, DLN2_MODULE_ADC)) {
        return DLN2_RES_FAIL;
    }

    err = adc_oneshot_read(s_adc.unit, s_adc_map[channel].channel, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read ch%u GPIO%d failed: %s", channel,
                 s_adc_map[channel].gpio, esp_err_to_name(err));
        return DLN2_RES_FAIL;
    }

    if (raw < 0) {
        raw = 0;
    }

    raw >>= 2;
    if (raw > DLN2_ADC_RAW_MAX_10BIT) {
        raw = DLN2_ADC_RAW_MAX_10BIT;
    }

    *value = (uint16_t)raw;
    return DLN2_RES_SUCCESS;
}

static size_t adc_get_value(const dln2_request_t *request, uint8_t *response,
                            size_t response_cap)
{
    uint8_t port;
    uint8_t channel;
    uint16_t value = 0;
    uint8_t payload[sizeof(uint16_t)];
    uint16_t result = adc_parse_channel(request, &port, &channel);

    (void)port;

    if (result == DLN2_RES_SUCCESS) {
        result = adc_read_channel(channel, &value);
    }

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    dln2_put_le16(payload, value);
    ESP_LOGI(TAG, "ADC ch%u GPIO%d raw10=%u", channel,
             s_adc_map[channel].gpio, value);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(payload), response, response_cap);
}

static size_t adc_get_all_values(const dln2_request_t *request,
                                 uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint8_t payload[sizeof(uint16_t) *
                    (1 + DLN2_ADC_ALL_VALUES_MAX_CHANNELS)] = { 0 };
    uint16_t channel_mask = 0;
    uint16_t result = adc_parse_port(request, &port);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    for (uint8_t i = 0; i < DLN2_ADC_CHANNEL_COUNT; i++) {
        uint16_t value = 0;

        if (!s_adc_channel_enabled[i]) {
            continue;
        }

        result = adc_read_channel(i, &value);
        if (result != DLN2_RES_SUCCESS) {
            return dln2_make_response(request, result, NULL, 0, response,
                                      response_cap);
        }

        channel_mask |= BIT(i);
        dln2_put_le16(payload + sizeof(uint16_t) * (1 + i), value);
    }

    dln2_put_le16(payload, channel_mask);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(payload), response, response_cap);
}

void dln2_adc_init(void)
{
    memset(&s_adc, 0, sizeof(s_adc));
    memset(s_adc_channel_enabled, 0, sizeof(s_adc_channel_enabled));
    s_adc.resolution_bits = DLN2_ADC_RESOLUTION_BITS;

    ESP_LOGI(TAG,
             "initialized ADC port %u channels: ch0=GPIO14 ch1=GPIO15 "
             "ch2=GPIO16 ch3=GPIO17",
             DLN2_ADC_PORT);
}

size_t dln2_adc_handle_request(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap)
{
    switch (request->id) {
    case DLN2_ADC_GET_CHANNEL_COUNT:
        return adc_get_channel_count(request, response, response_cap);

    case DLN2_ADC_ENABLE:
        return adc_enable_request(request, true, response, response_cap);

    case DLN2_ADC_DISABLE:
        return adc_enable_request(request, false, response, response_cap);

    case DLN2_ADC_CHANNEL_ENABLE:
        return adc_channel_enable_request(request, true, response,
                                          response_cap);

    case DLN2_ADC_CHANNEL_DISABLE:
        return adc_channel_enable_request(request, false, response,
                                          response_cap);

    case DLN2_ADC_SET_RESOLUTION:
        return adc_set_resolution(request, response, response_cap);

    case DLN2_ADC_CHANNEL_GET_VAL:
        return adc_get_value(request, response, response_cap);

    case DLN2_ADC_CHANNEL_GET_ALL_VAL:
        return adc_get_all_values(request, response, response_cap);

    default:
        ESP_LOGW(TAG, "unsupported ADC id=0x%04x", request->id);
        return dln2_make_response(request, DLN2_RES_NOT_IMPLEMENTED, NULL, 0,
                                  response, response_cap);
    }
}
