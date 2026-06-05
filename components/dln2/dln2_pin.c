#include "dln2_private.h"

#include <string.h>

#include "esp_log.h"

#define DLN2_ESP32S3_GPIO_COUNT 49
#define DLN2_PIN_OWNER_FREE 0x00
#define DLN2_PIN_OWNER_UNAVAILABLE 0xff

typedef struct {
    uint8_t owner;
} dln2_pin_state_t;

static const char *TAG = "dln2_pin";
static dln2_pin_state_t s_pin_state[DLN2_ESP32S3_GPIO_COUNT];

static const uint8_t s_unavailable_gpios[] = {
    0,  /* boot strapping pin and BOOT key */
    3,  /* JTAG source strapping pin */
    19, /* native USB D- */
    20, /* native USB D+ */
    35, /* OSPI PSRAM on ESP32-S3R8 modules */
    36, /* OSPI PSRAM on ESP32-S3R8 modules */
    37, /* OSPI PSRAM on ESP32-S3R8 modules */
    39, /* JTAG group, reserved */
    40, /* JTAG group, reserved */
    41, /* JTAG group, reserved */
    42, /* JTAG group, reserved */
    43, /* UART0 TX, flash/log path */
    44, /* UART0 RX, flash/log path */
    45, /* strapping pin and USER key path */
    46, /* strapping pin */
    48, /* onboard WS2812 LED path */
};

const char *dln2_module_name(uint8_t module)
{
    switch (module) {
    case DLN2_MODULE_GPIO:
        return "GPIO";
    case DLN2_MODULE_I2C:
        return "I2C";
    case DLN2_MODULE_SPI:
        return "SPI";
    case DLN2_MODULE_ADC:
        return "ADC";
    case DLN2_PIN_OWNER_UNAVAILABLE:
        return "unavailable";
    case DLN2_PIN_OWNER_FREE:
        return "free";
    default:
        return "unknown";
    }
}

void dln2_pin_init(void)
{
    memset(s_pin_state, 0, sizeof(s_pin_state));

    for (size_t i = 0; i < sizeof(s_unavailable_gpios); i++) {
        s_pin_state[s_unavailable_gpios[i]].owner = DLN2_PIN_OWNER_UNAVAILABLE;
    }

    ESP_LOGI(TAG, "initialized ESP32-S3 pin ownership map");
}

uint16_t dln2_pin_claim(int gpio, uint8_t module)
{
    dln2_pin_state_t *state;

    if (gpio < 0 || gpio >= DLN2_ESP32S3_GPIO_COUNT) {
        return DLN2_RES_INVALID_PIN_NUMBER;
    }

    state = &s_pin_state[gpio];
    if (state->owner == DLN2_PIN_OWNER_UNAVAILABLE) {
        ESP_LOGW(TAG, "GPIO%d is unavailable for %s", gpio,
                 dln2_module_name(module));
        return DLN2_RES_PIN_NOT_CONNECTED_TO_MODULE;
    }

    if (state->owner != DLN2_PIN_OWNER_FREE && state->owner != module) {
        ESP_LOGW(TAG, "GPIO%d owned by %s, requested by %s", gpio,
                 dln2_module_name(state->owner), dln2_module_name(module));
        return DLN2_RES_PIN_IN_USE;
    }

    state->owner = module;
    ESP_LOGI(TAG, "GPIO%d claimed by %s", gpio, dln2_module_name(module));
    return DLN2_RES_SUCCESS;
}

uint16_t dln2_pin_release(int gpio, uint8_t module)
{
    dln2_pin_state_t *state;

    if (gpio < 0 || gpio >= DLN2_ESP32S3_GPIO_COUNT) {
        return DLN2_RES_INVALID_PIN_NUMBER;
    }

    state = &s_pin_state[gpio];
    if (state->owner == DLN2_PIN_OWNER_UNAVAILABLE) {
        return DLN2_RES_PIN_NOT_CONNECTED_TO_MODULE;
    }

    if (state->owner == DLN2_PIN_OWNER_FREE) {
        return DLN2_RES_SUCCESS;
    }

    if (state->owner != module) {
        ESP_LOGW(TAG, "GPIO%d release by %s rejected; owner is %s", gpio,
                 dln2_module_name(module), dln2_module_name(state->owner));
        return DLN2_RES_PIN_NOT_CONNECTED_TO_MODULE;
    }

    state->owner = DLN2_PIN_OWNER_FREE;
    ESP_LOGI(TAG, "GPIO%d released by %s", gpio, dln2_module_name(module));
    return DLN2_RES_SUCCESS;
}

bool dln2_pin_is_claimed(int gpio, uint8_t module)
{
    if (gpio < 0 || gpio >= DLN2_ESP32S3_GPIO_COUNT) {
        return false;
    }

    return s_pin_state[gpio].owner == module;
}
