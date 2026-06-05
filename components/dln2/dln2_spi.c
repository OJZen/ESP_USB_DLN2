#include "dln2_private.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#define DLN2_SPI_CMD(cmd) DLN2_CMD(cmd, DLN2_MODULE_SPI)

#define DLN2_SPI_ENABLE                    DLN2_SPI_CMD(0x11)
#define DLN2_SPI_DISABLE                   DLN2_SPI_CMD(0x12)
#define DLN2_SPI_SET_MODE                  DLN2_SPI_CMD(0x14)
#define DLN2_SPI_SET_FRAME_SIZE            DLN2_SPI_CMD(0x16)
#define DLN2_SPI_SET_FREQUENCY             DLN2_SPI_CMD(0x18)
#define DLN2_SPI_READ_WRITE                DLN2_SPI_CMD(0x1a)
#define DLN2_SPI_READ                      DLN2_SPI_CMD(0x1b)
#define DLN2_SPI_WRITE                     DLN2_SPI_CMD(0x1c)
#define DLN2_SPI_SET_SS                    DLN2_SPI_CMD(0x26)
#define DLN2_SPI_SS_MULTI_ENABLE           DLN2_SPI_CMD(0x38)
#define DLN2_SPI_SS_MULTI_DISABLE          DLN2_SPI_CMD(0x39)
#define DLN2_SPI_GET_SUPPORTED_FRAME_SIZES DLN2_SPI_CMD(0x43)
#define DLN2_SPI_GET_SS_COUNT              DLN2_SPI_CMD(0x44)
#define DLN2_SPI_GET_MIN_FREQUENCY         DLN2_SPI_CMD(0x45)
#define DLN2_SPI_GET_MAX_FREQUENCY         DLN2_SPI_CMD(0x46)

#define DLN2_SPI_PORT 0
#define DLN2_SPI_HOST SPI2_HOST
#define DLN2_SPI_SCLK_GPIO GPIO_NUM_10
#define DLN2_SPI_MOSI_GPIO GPIO_NUM_11
#define DLN2_SPI_MISO_GPIO GPIO_NUM_12
#define DLN2_SPI_CS_GPIO GPIO_NUM_13
#define DLN2_SPI_DEFAULT_SPEED_HZ 1000000U
#define DLN2_SPI_MIN_SPEED_HZ 10000U
#define DLN2_SPI_MAX_SPEED_HZ 10000000U
#define DLN2_SPI_MAX_XFER_SIZE 256
#define DLN2_SPI_ATTR_LEAVE_SS_LOW BIT(0)
#define DLN2_SPI_FRAME_SIZE_8_BITS 8
#define DLN2_SPI_MODE_MASK 0x03

typedef struct {
    bool enabled;
    bool cs_enabled;
    bool cs_active;
    uint32_t speed_hz;
    uint8_t mode;
    uint8_t frame_size;
    spi_device_handle_t dev;
} dln2_spi_state_t;

static const char *TAG = "dln2_spi";
static dln2_spi_state_t s_spi;

static uint16_t spi_parse_port(const dln2_request_t *request, uint8_t *port)
{
    if (request->payload_len != sizeof(*port)) {
        return DLN2_RES_INVALID_COMMAND_SIZE;
    }

    *port = request->payload[0];
    if (*port != DLN2_SPI_PORT) {
        return DLN2_RES_INVALID_PORT_NUMBER;
    }

    return DLN2_RES_SUCCESS;
}

static uint32_t spi_clamp_speed(uint32_t speed_hz)
{
    if (speed_hz < DLN2_SPI_MIN_SPEED_HZ) {
        return DLN2_SPI_MIN_SPEED_HZ;
    }

    if (speed_hz > DLN2_SPI_MAX_SPEED_HZ) {
        return DLN2_SPI_MAX_SPEED_HZ;
    }

    return speed_hz;
}

static void spi_cs_set_active(bool active)
{
    if (!s_spi.cs_enabled || s_spi.cs_active == active) {
        return;
    }

    gpio_set_level(DLN2_SPI_CS_GPIO, active ? 0 : 1);
    s_spi.cs_active = active;
}

static uint16_t spi_claim_bus_pins(void)
{
    uint16_t result;

    result = dln2_pin_claim(DLN2_SPI_SCLK_GPIO, DLN2_MODULE_SPI);
    if (result != DLN2_RES_SUCCESS) {
        return result;
    }

    result = dln2_pin_claim(DLN2_SPI_MOSI_GPIO, DLN2_MODULE_SPI);
    if (result != DLN2_RES_SUCCESS) {
        dln2_pin_release(DLN2_SPI_SCLK_GPIO, DLN2_MODULE_SPI);
        return result;
    }

    result = dln2_pin_claim(DLN2_SPI_MISO_GPIO, DLN2_MODULE_SPI);
    if (result != DLN2_RES_SUCCESS) {
        dln2_pin_release(DLN2_SPI_MOSI_GPIO, DLN2_MODULE_SPI);
        dln2_pin_release(DLN2_SPI_SCLK_GPIO, DLN2_MODULE_SPI);
        return result;
    }

    return DLN2_RES_SUCCESS;
}

static void spi_release_bus_pins(void)
{
    dln2_pin_release(DLN2_SPI_MISO_GPIO, DLN2_MODULE_SPI);
    dln2_pin_release(DLN2_SPI_MOSI_GPIO, DLN2_MODULE_SPI);
    dln2_pin_release(DLN2_SPI_SCLK_GPIO, DLN2_MODULE_SPI);
}

static uint16_t spi_enable_bus(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = DLN2_SPI_MOSI_GPIO,
        .miso_io_num = DLN2_SPI_MISO_GPIO,
        .sclk_io_num = DLN2_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DLN2_SPI_MAX_XFER_SIZE,
    };
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = (int)s_spi.speed_hz,
        .mode = s_spi.mode & DLN2_SPI_MODE_MASK,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    uint16_t result;
    esp_err_t err;

    if (s_spi.enabled) {
        return DLN2_RES_SUCCESS;
    }

    if (!s_spi.cs_enabled) {
        return DLN2_RES_SPI_MASTER_INVALID_SS_VALUE;
    }

    result = spi_claim_bus_pins();
    if (result != DLN2_RES_SUCCESS) {
        return result;
    }

    err = spi_bus_initialize(DLN2_SPI_HOST, &bus_config, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        spi_release_bus_pins();
        return DLN2_RES_FAIL;
    }

    err = spi_bus_add_device(DLN2_SPI_HOST, &dev_config, &s_spi.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(DLN2_SPI_HOST);
        spi_release_bus_pins();
        return DLN2_RES_FAIL;
    }

    s_spi.enabled = true;
    s_spi.cs_active = false;
    gpio_set_level(DLN2_SPI_CS_GPIO, 1);

    ESP_LOGI(TAG,
             "enabled SPI port %u SCLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d "
             "CS=GPIO%d speed=%u mode=%u",
             DLN2_SPI_PORT, DLN2_SPI_SCLK_GPIO, DLN2_SPI_MOSI_GPIO,
             DLN2_SPI_MISO_GPIO, DLN2_SPI_CS_GPIO, (unsigned)s_spi.speed_hz,
             s_spi.mode);

    return DLN2_RES_SUCCESS;
}

static uint16_t spi_disable_bus(void)
{
    if (!s_spi.enabled) {
        return DLN2_RES_SUCCESS;
    }

    spi_cs_set_active(false);

    esp_err_t err = spi_bus_remove_device(s_spi.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_remove_device failed: %s",
                 esp_err_to_name(err));
        return DLN2_RES_FAIL;
    }

    s_spi.dev = NULL;

    err = spi_bus_free(DLN2_SPI_HOST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_free failed: %s", esp_err_to_name(err));
        return DLN2_RES_FAIL;
    }

    s_spi.enabled = false;
    spi_release_bus_pins();

    ESP_LOGI(TAG, "disabled SPI port %u", DLN2_SPI_PORT);
    return DLN2_RES_SUCCESS;
}

static size_t spi_enable_request(const dln2_request_t *request, bool enable,
                                 uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint16_t result;

    if (enable) {
        result = spi_parse_port(request, &port);
    } else {
        if (request->payload_len != 2) {
            return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                      NULL, 0, response, response_cap);
        }
        port = request->payload[0];
        result = port == DLN2_SPI_PORT ? DLN2_RES_SUCCESS :
                 DLN2_RES_INVALID_PORT_NUMBER;
    }

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    result = enable ? spi_enable_bus() : spi_disable_bus();
    return dln2_make_response(request, result, NULL, 0, response,
                              response_cap);
}

static size_t spi_u16_response(const dln2_request_t *request, uint16_t value,
                               uint8_t *response, size_t response_cap)
{
    uint8_t payload[2];

    dln2_put_le16(payload, value);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(payload), response, response_cap);
}

static size_t spi_u32_response(const dln2_request_t *request, uint32_t value,
                               uint8_t *response, size_t response_cap)
{
    uint8_t payload[4];

    dln2_put_le32(payload, value);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(payload), response, response_cap);
}

static size_t spi_get_ss_count(const dln2_request_t *request, uint8_t *response,
                               size_t response_cap)
{
    uint8_t port;
    uint16_t result = spi_parse_port(request, &port);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    return spi_u16_response(request, 1, response, response_cap);
}

static size_t spi_get_frequency(const dln2_request_t *request, uint32_t speed,
                                uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint16_t result = spi_parse_port(request, &port);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    return spi_u32_response(request, speed, response, response_cap);
}

static size_t spi_get_supported_frame_sizes(const dln2_request_t *request,
                                            uint8_t *response,
                                            size_t response_cap)
{
    uint8_t payload[37] = { 0 };
    uint8_t port;
    uint16_t result = spi_parse_port(request, &port);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    payload[0] = 1;
    payload[1] = DLN2_SPI_FRAME_SIZE_8_BITS;
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(payload), response, response_cap);
}

static size_t spi_set_frequency(const dln2_request_t *request,
                                uint8_t *response, size_t response_cap)
{
    uint8_t payload[4];
    uint8_t port;
    uint32_t speed;

    if (request->payload_len != 5) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    if (port != DLN2_SPI_PORT) {
        return dln2_make_response(request, DLN2_RES_INVALID_PORT_NUMBER,
                                  NULL, 0, response, response_cap);
    }

    speed = spi_clamp_speed(dln2_get_le32(request->payload + 1));
    s_spi.speed_hz = speed;

    dln2_put_le32(payload, speed);
    ESP_LOGI(TAG, "set frequency=%u", (unsigned)s_spi.speed_hz);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(payload), response, response_cap);
}

static size_t spi_set_mode(const dln2_request_t *request, uint8_t *response,
                           size_t response_cap)
{
    uint8_t port;
    uint8_t mode;

    if (request->payload_len != 2) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    mode = request->payload[1];
    if (port != DLN2_SPI_PORT) {
        return dln2_make_response(request, DLN2_RES_INVALID_PORT_NUMBER,
                                  NULL, 0, response, response_cap);
    }

    if ((mode & ~DLN2_SPI_MODE_MASK) != 0) {
        return dln2_make_response(request, DLN2_RES_INVALID_MODE, NULL, 0,
                                  response, response_cap);
    }

    s_spi.mode = mode;
    ESP_LOGI(TAG, "set mode=%u", s_spi.mode);
    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static size_t spi_set_frame_size(const dln2_request_t *request,
                                 uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint8_t frame_size;

    if (request->payload_len != 2) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    frame_size = request->payload[1];
    if (port != DLN2_SPI_PORT) {
        return dln2_make_response(request, DLN2_RES_INVALID_PORT_NUMBER,
                                  NULL, 0, response, response_cap);
    }

    if (frame_size != DLN2_SPI_FRAME_SIZE_8_BITS) {
        return dln2_make_response(request, DLN2_RES_INVALID_VALUE, NULL, 0,
                                  response, response_cap);
    }

    s_spi.frame_size = frame_size;
    ESP_LOGI(TAG, "set frame_size=%u", s_spi.frame_size);
    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static size_t spi_set_ss(const dln2_request_t *request, uint8_t *response,
                         size_t response_cap)
{
    uint8_t port;
    uint8_t cs_mask;

    if (request->payload_len != 2) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    cs_mask = request->payload[1];
    if (port != DLN2_SPI_PORT) {
        return dln2_make_response(request, DLN2_RES_INVALID_PORT_NUMBER,
                                  NULL, 0, response, response_cap);
    }

    if (cs_mask != 0xfe) {
        return dln2_make_response(request,
                                  DLN2_RES_SPI_MASTER_INVALID_SS_VALUE,
                                  NULL, 0, response, response_cap);
    }

    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static size_t spi_ss_multi_enable(const dln2_request_t *request, bool enable,
                                  uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint8_t cs_mask;
    uint16_t result;
    esp_err_t err;

    if (request->payload_len != 2) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    cs_mask = request->payload[1];
    if (port != DLN2_SPI_PORT) {
        return dln2_make_response(request, DLN2_RES_INVALID_PORT_NUMBER,
                                  NULL, 0, response, response_cap);
    }

    if (cs_mask != 0x01) {
        return dln2_make_response(request,
                                  DLN2_RES_SPI_MASTER_INVALID_SS_VALUE,
                                  NULL, 0, response, response_cap);
    }

    if (enable) {
        if (s_spi.cs_enabled) {
            return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0,
                                      response, response_cap);
        }

        result = dln2_pin_claim(DLN2_SPI_CS_GPIO, DLN2_MODULE_SPI);
        if (result != DLN2_RES_SUCCESS) {
            return dln2_make_response(request, result, NULL, 0, response,
                                      response_cap);
        }

        err = gpio_reset_pin(DLN2_SPI_CS_GPIO);
        if (err == ESP_OK) {
            err = gpio_set_direction(DLN2_SPI_CS_GPIO, GPIO_MODE_OUTPUT);
        }
        if (err == ESP_OK) {
            err = gpio_set_level(DLN2_SPI_CS_GPIO, 1);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "configure CS GPIO%d failed: %s", DLN2_SPI_CS_GPIO,
                     esp_err_to_name(err));
            dln2_pin_release(DLN2_SPI_CS_GPIO, DLN2_MODULE_SPI);
            return dln2_make_response(request, DLN2_RES_FAIL, NULL, 0,
                                      response, response_cap);
        }

        s_spi.cs_enabled = true;
        s_spi.cs_active = false;
    } else {
        if (s_spi.enabled) {
            result = spi_disable_bus();
            if (result != DLN2_RES_SUCCESS) {
                return dln2_make_response(request, result, NULL, 0, response,
                                          response_cap);
            }
        }

        spi_cs_set_active(false);
        gpio_reset_pin(DLN2_SPI_CS_GPIO);
        result = dln2_pin_release(DLN2_SPI_CS_GPIO, DLN2_MODULE_SPI);
        if (result != DLN2_RES_SUCCESS) {
            return dln2_make_response(request, result, NULL, 0, response,
                                      response_cap);
        }
        s_spi.cs_enabled = false;
        s_spi.cs_active = false;
    }

    ESP_LOGI(TAG, "%s CS mask=0x%02x", enable ? "enabled" : "disabled",
             cs_mask);
    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static uint16_t spi_check_transfer(uint8_t port, uint16_t size)
{
    if (port != DLN2_SPI_PORT) {
        return DLN2_RES_INVALID_PORT_NUMBER;
    }

    if (!s_spi.enabled || !s_spi.cs_enabled) {
        return DLN2_RES_FAIL;
    }

    if (s_spi.frame_size != DLN2_SPI_FRAME_SIZE_8_BITS) {
        return DLN2_RES_INVALID_VALUE;
    }

    if (size > DLN2_SPI_MAX_XFER_SIZE) {
        return DLN2_RES_INVALID_BUFFER_SIZE;
    }

    return DLN2_RES_SUCCESS;
}

static uint16_t spi_transmit(const uint8_t *tx_data, uint8_t *rx_data,
                             uint16_t size)
{
    spi_transaction_t transaction = {
        .length = (size_t)size * 8,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    esp_err_t err;

    if (size == 0) {
        return DLN2_RES_SUCCESS;
    }

    err = spi_device_transmit(s_spi.dev, &transaction);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi_device_transmit len=%u failed: %s", size,
                 esp_err_to_name(err));
        return DLN2_RES_FAIL;
    }

    return DLN2_RES_SUCCESS;
}

static size_t spi_read_write_request(const dln2_request_t *request,
                                     uint8_t *response, size_t response_cap)
{
    uint8_t payload[sizeof(uint16_t) + DLN2_SPI_MAX_XFER_SIZE];
    uint8_t port;
    uint8_t attr;
    uint16_t size;
    uint16_t result;

    if (request->payload_len < 4) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    size = dln2_get_le16(request->payload + 1);
    attr = request->payload[3];
    if (request->payload_len != 4 + size) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    result = spi_check_transfer(port, size);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    dln2_put_le16(payload, size);
    spi_cs_set_active(true);
    result = spi_transmit(request->payload + 4, payload + sizeof(uint16_t),
                          size);
    if (!(attr & DLN2_SPI_ATTR_LEAVE_SS_LOW)) {
        spi_cs_set_active(false);
    }

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    ESP_LOGI(TAG, "read_write len=%u attr=0x%02x", size, attr);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(uint16_t) + size, response,
                              response_cap);
}

static size_t spi_read_request(const dln2_request_t *request, uint8_t *response,
                               size_t response_cap)
{
    uint8_t payload[sizeof(uint16_t) + DLN2_SPI_MAX_XFER_SIZE];
    uint8_t port;
    uint8_t attr;
    uint16_t size;
    uint16_t result;

    if (request->payload_len != 4) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    size = dln2_get_le16(request->payload + 1);
    attr = request->payload[3];
    result = spi_check_transfer(port, size);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    dln2_put_le16(payload, size);
    memset(payload + sizeof(uint16_t), 0, size);
    spi_cs_set_active(true);
    result = spi_transmit(NULL, payload + sizeof(uint16_t), size);
    if (!(attr & DLN2_SPI_ATTR_LEAVE_SS_LOW)) {
        spi_cs_set_active(false);
    }

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    ESP_LOGI(TAG, "read len=%u attr=0x%02x", size, attr);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(uint16_t) + size, response,
                              response_cap);
}

static size_t spi_write_request(const dln2_request_t *request,
                                uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint8_t attr;
    uint16_t size;
    uint16_t result;

    if (request->payload_len < 4) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    size = dln2_get_le16(request->payload + 1);
    attr = request->payload[3];
    if (request->payload_len != 4 + size) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    result = spi_check_transfer(port, size);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    spi_cs_set_active(true);
    result = spi_transmit(request->payload + 4, NULL, size);
    if (!(attr & DLN2_SPI_ATTR_LEAVE_SS_LOW)) {
        spi_cs_set_active(false);
    }

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    ESP_LOGI(TAG, "write len=%u attr=0x%02x", size, attr);
    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

void dln2_spi_init(void)
{
    memset(&s_spi, 0, sizeof(s_spi));
    s_spi.speed_hz = DLN2_SPI_DEFAULT_SPEED_HZ;
    s_spi.frame_size = DLN2_SPI_FRAME_SIZE_8_BITS;
    ESP_LOGI(TAG,
             "initialized SPI port %u SCLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d "
             "CS=GPIO%d",
             DLN2_SPI_PORT, DLN2_SPI_SCLK_GPIO, DLN2_SPI_MOSI_GPIO,
             DLN2_SPI_MISO_GPIO, DLN2_SPI_CS_GPIO);
}

size_t dln2_spi_handle_request(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap)
{
    switch (request->id) {
    case DLN2_SPI_ENABLE:
        return spi_enable_request(request, true, response, response_cap);

    case DLN2_SPI_DISABLE:
        return spi_enable_request(request, false, response, response_cap);

    case DLN2_SPI_GET_SS_COUNT:
        return spi_get_ss_count(request, response, response_cap);

    case DLN2_SPI_GET_MIN_FREQUENCY:
        return spi_get_frequency(request, DLN2_SPI_MIN_SPEED_HZ, response,
                                 response_cap);

    case DLN2_SPI_GET_MAX_FREQUENCY:
        return spi_get_frequency(request, DLN2_SPI_MAX_SPEED_HZ, response,
                                 response_cap);

    case DLN2_SPI_GET_SUPPORTED_FRAME_SIZES:
        return spi_get_supported_frame_sizes(request, response, response_cap);

    case DLN2_SPI_SS_MULTI_ENABLE:
        return spi_ss_multi_enable(request, true, response, response_cap);

    case DLN2_SPI_SS_MULTI_DISABLE:
        return spi_ss_multi_enable(request, false, response, response_cap);

    case DLN2_SPI_SET_SS:
        return spi_set_ss(request, response, response_cap);

    case DLN2_SPI_SET_FREQUENCY:
        return spi_set_frequency(request, response, response_cap);

    case DLN2_SPI_SET_MODE:
        return spi_set_mode(request, response, response_cap);

    case DLN2_SPI_SET_FRAME_SIZE:
        return spi_set_frame_size(request, response, response_cap);

    case DLN2_SPI_READ_WRITE:
        return spi_read_write_request(request, response, response_cap);

    case DLN2_SPI_READ:
        return spi_read_request(request, response, response_cap);

    case DLN2_SPI_WRITE:
        return spi_write_request(request, response, response_cap);

    default:
        ESP_LOGW(TAG, "unsupported SPI id=0x%04x", request->id);
        return dln2_make_response(request, DLN2_RES_NOT_IMPLEMENTED, NULL, 0,
                                  response, response_cap);
    }
}
