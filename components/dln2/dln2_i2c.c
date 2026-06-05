#include "dln2_private.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define DLN2_I2C_CMD(cmd) DLN2_CMD(cmd, DLN2_MODULE_I2C)

#define DLN2_I2C_ENABLE  DLN2_I2C_CMD(0x01)
#define DLN2_I2C_DISABLE DLN2_I2C_CMD(0x02)
#define DLN2_I2C_WRITE   DLN2_I2C_CMD(0x06)
#define DLN2_I2C_READ    DLN2_I2C_CMD(0x07)

#define DLN2_I2C_PORT 0
#define DLN2_I2C_SPEED_HZ 100000
#define DLN2_I2C_TIMEOUT_MS 150
#define DLN2_I2C_MAX_XFER_SIZE 256
#define DLN2_I2C_MSG_HEADER_LEN 9
#define DLN2_I2C_SDA_GPIO GPIO_NUM_38
#define DLN2_I2C_SCL_GPIO GPIO_NUM_47

typedef struct {
    bool enabled;
    i2c_master_bus_handle_t bus;
} dln2_i2c_state_t;

static const char *TAG = "dln2_i2c";
static dln2_i2c_state_t s_i2c;

static uint16_t i2c_parse_port(const dln2_request_t *request, uint8_t *port)
{
    if (request->payload_len != sizeof(*port)) {
        return DLN2_RES_INVALID_COMMAND_SIZE;
    }

    *port = request->payload[0];
    if (*port != DLN2_I2C_PORT) {
        return DLN2_RES_INVALID_PORT_NUMBER;
    }

    return DLN2_RES_SUCCESS;
}

static uint16_t i2c_add_device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = DLN2_I2C_SPEED_HZ,
    };
    esp_err_t err;

    if (addr > 0x7f) {
        return DLN2_RES_BAD_PARAMETER;
    }

    err = i2c_master_bus_add_device(s_i2c.bus, &dev_config, dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device addr=0x%02x failed: %s", addr,
                 esp_err_to_name(err));
        return DLN2_RES_FAIL;
    }

    return DLN2_RES_SUCCESS;
}

static uint16_t i2c_remove_device(i2c_master_dev_handle_t dev)
{
    esp_err_t err = i2c_master_bus_rm_device(dev);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remove device failed: %s", esp_err_to_name(err));
        return DLN2_RES_FAIL;
    }

    return DLN2_RES_SUCCESS;
}

static uint16_t i2c_enable(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = DLN2_I2C_SDA_GPIO,
        .scl_io_num = DLN2_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    uint16_t result;
    esp_err_t err;

    if (s_i2c.enabled) {
        return DLN2_RES_SUCCESS;
    }

    result = dln2_pin_claim(DLN2_I2C_SCL_GPIO, DLN2_MODULE_I2C);
    if (result != DLN2_RES_SUCCESS) {
        return result;
    }

    result = dln2_pin_claim(DLN2_I2C_SDA_GPIO, DLN2_MODULE_I2C);
    if (result != DLN2_RES_SUCCESS) {
        dln2_pin_release(DLN2_I2C_SCL_GPIO, DLN2_MODULE_I2C);
        return result;
    }

    err = i2c_new_master_bus(&bus_config, &s_i2c.bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        dln2_pin_release(DLN2_I2C_SDA_GPIO, DLN2_MODULE_I2C);
        dln2_pin_release(DLN2_I2C_SCL_GPIO, DLN2_MODULE_I2C);
        return DLN2_RES_FAIL;
    }

    s_i2c.enabled = true;
    ESP_LOGI(TAG, "enabled I2C port %u SDA=GPIO%d SCL=GPIO%d speed=%u",
             DLN2_I2C_PORT, DLN2_I2C_SDA_GPIO, DLN2_I2C_SCL_GPIO,
             DLN2_I2C_SPEED_HZ);

    return DLN2_RES_SUCCESS;
}

static uint16_t i2c_disable(void)
{
    uint16_t result;

    if (s_i2c.enabled) {
        esp_err_t err = i2c_del_master_bus(s_i2c.bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_del_master_bus failed: %s",
                     esp_err_to_name(err));
            return DLN2_RES_FAIL;
        }

        s_i2c.bus = NULL;
        s_i2c.enabled = false;
    }

    result = dln2_pin_release(DLN2_I2C_SDA_GPIO, DLN2_MODULE_I2C);
    if (result != DLN2_RES_SUCCESS) {
        return result;
    }

    result = dln2_pin_release(DLN2_I2C_SCL_GPIO, DLN2_MODULE_I2C);
    if (result != DLN2_RES_SUCCESS) {
        return result;
    }

    ESP_LOGI(TAG, "disabled I2C port %u", DLN2_I2C_PORT);
    return DLN2_RES_SUCCESS;
}

static size_t i2c_enable_request(const dln2_request_t *request, bool enable,
                                 uint8_t *response, size_t response_cap)
{
    uint8_t port;
    uint16_t result = i2c_parse_port(request, &port);

    (void)port;

    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    result = enable ? i2c_enable() : i2c_disable();
    return dln2_make_response(request, result, NULL, 0, response,
                              response_cap);
}

static uint16_t i2c_check_runtime_request(uint8_t port, uint8_t addr,
                                          uint8_t mem_addr_len,
                                          uint16_t buf_len)
{
    if (port != DLN2_I2C_PORT) {
        return DLN2_RES_INVALID_PORT_NUMBER;
    }

    if (!s_i2c.enabled) {
        return DLN2_RES_FAIL;
    }

    if (addr > 0x7f) {
        return DLN2_RES_BAD_PARAMETER;
    }

    if (mem_addr_len != 0) {
        return DLN2_RES_INVALID_VALUE;
    }

    if (buf_len > DLN2_I2C_MAX_XFER_SIZE) {
        return DLN2_RES_INVALID_BUFFER_SIZE;
    }

    return DLN2_RES_SUCCESS;
}

static size_t i2c_write_request(const dln2_request_t *request,
                                uint8_t *response, size_t response_cap)
{
    i2c_master_dev_handle_t dev = NULL;
    uint8_t port;
    uint8_t addr;
    uint8_t mem_addr_len;
    uint16_t buf_len;
    uint16_t result;
    esp_err_t err;

    if (request->payload_len < DLN2_I2C_MSG_HEADER_LEN) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    addr = request->payload[1];
    mem_addr_len = request->payload[2];
    buf_len = dln2_get_le16(request->payload + 7);

    if (request->payload_len != DLN2_I2C_MSG_HEADER_LEN + buf_len) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    result = i2c_check_runtime_request(port, addr, mem_addr_len, buf_len);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    if (buf_len == 0) {
        err = i2c_master_probe(s_i2c.bus, addr, DLN2_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "probe addr=0x%02x failed: %s", addr,
                     esp_err_to_name(err));
            return dln2_make_response(
                request, DLN2_RES_I2C_MASTER_SENDING_ADDRESS_FAILED, NULL, 0,
                response, response_cap);
        }

        ESP_LOGI(TAG, "probe addr=0x%02x", addr);
        return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0,
                                  response, response_cap);
    }

    result = i2c_add_device(addr, &dev);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    err = i2c_master_transmit(dev, request->payload + DLN2_I2C_MSG_HEADER_LEN,
                              buf_len, DLN2_I2C_TIMEOUT_MS);
    result = i2c_remove_device(dev);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write addr=0x%02x len=%u failed: %s", addr, buf_len,
                 esp_err_to_name(err));
        return dln2_make_response(request,
                                  DLN2_RES_I2C_MASTER_SENDING_ADDRESS_FAILED,
                                  NULL, 0, response, response_cap);
    }

    ESP_LOGI(TAG, "write addr=0x%02x len=%u", addr, buf_len);
    return dln2_make_response(request, DLN2_RES_SUCCESS, NULL, 0, response,
                              response_cap);
}

static size_t i2c_read_request(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap)
{
    uint8_t payload[sizeof(uint16_t) + DLN2_I2C_MAX_XFER_SIZE];
    i2c_master_dev_handle_t dev = NULL;
    uint8_t port;
    uint8_t addr;
    uint8_t mem_addr_len;
    uint16_t buf_len;
    uint16_t result;
    esp_err_t err;

    if (request->payload_len != DLN2_I2C_MSG_HEADER_LEN) {
        return dln2_make_response(request, DLN2_RES_INVALID_COMMAND_SIZE,
                                  NULL, 0, response, response_cap);
    }

    port = request->payload[0];
    addr = request->payload[1];
    mem_addr_len = request->payload[2];
    buf_len = dln2_get_le16(request->payload + 7);

    result = i2c_check_runtime_request(port, addr, mem_addr_len, buf_len);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    if (buf_len == 0) {
        dln2_put_le16(payload, 0);
        return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                                  sizeof(uint16_t), response, response_cap);
    }

    result = i2c_add_device(addr, &dev);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    dln2_put_le16(payload, buf_len);
    err = i2c_master_receive(dev, payload + sizeof(uint16_t), buf_len,
                             DLN2_I2C_TIMEOUT_MS);
    result = i2c_remove_device(dev);
    if (result != DLN2_RES_SUCCESS) {
        return dln2_make_response(request, result, NULL, 0, response,
                                  response_cap);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read addr=0x%02x len=%u failed: %s", addr, buf_len,
                 esp_err_to_name(err));
        return dln2_make_response(request,
                                  DLN2_RES_I2C_MASTER_SENDING_ADDRESS_FAILED,
                                  NULL, 0, response, response_cap);
    }

    ESP_LOGI(TAG, "read addr=0x%02x len=%u", addr, buf_len);
    return dln2_make_response(request, DLN2_RES_SUCCESS, payload,
                              sizeof(uint16_t) + buf_len, response,
                              response_cap);
}

void dln2_i2c_init(void)
{
    memset(&s_i2c, 0, sizeof(s_i2c));
    ESP_LOGI(TAG, "initialized I2C port %u SDA=GPIO%d SCL=GPIO%d",
             DLN2_I2C_PORT, DLN2_I2C_SDA_GPIO, DLN2_I2C_SCL_GPIO);
}

size_t dln2_i2c_handle_request(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap)
{
    switch (request->id) {
    case DLN2_I2C_ENABLE:
        return i2c_enable_request(request, true, response, response_cap);

    case DLN2_I2C_DISABLE:
        return i2c_enable_request(request, false, response, response_cap);

    case DLN2_I2C_WRITE:
        return i2c_write_request(request, response, response_cap);

    case DLN2_I2C_READ:
        return i2c_read_request(request, response, response_cap);

    default:
        ESP_LOGW(TAG, "unsupported I2C id=0x%04x", request->id);
        return dln2_make_response(request, DLN2_RES_NOT_IMPLEMENTED,
                                  NULL, 0, response, response_cap);
    }
}
