#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DLN2_CMD(cmd, module_id) ((uint16_t)((cmd) | ((module_id) << 8)))

#define DLN2_MODULE_GENERIC 0x00
#define DLN2_MODULE_GPIO    0x01
#define DLN2_MODULE_SPI     0x02
#define DLN2_MODULE_I2C     0x03
#define DLN2_MODULE_ADC     0x06

#define DLN2_CMD_GET_DEVICE_VER DLN2_CMD(0x30, DLN2_MODULE_GENERIC)
#define DLN2_CMD_GET_DEVICE_SN  DLN2_CMD(0x31, DLN2_MODULE_GENERIC)

#define DLN2_HW_ID 0x00000200U

#define DLN2_RES_SUCCESS               0x00
#define DLN2_RES_FAIL                  0x83
#define DLN2_RES_BAD_PARAMETER         0x85
#define DLN2_RES_INVALID_COMMAND_SIZE  0x88
#define DLN2_RES_INVALID_MESSAGE_SIZE  0x8a
#define DLN2_RES_INVALID_HANDLE        0x8f
#define DLN2_RES_NOT_IMPLEMENTED       0x91
#define DLN2_RES_PIN_IN_USE            0xa5
#define DLN2_RES_INVALID_PORT_NUMBER   0xa8
#define DLN2_RES_PIN_NOT_CONNECTED_TO_MODULE 0xaa
#define DLN2_RES_INVALID_PIN_NUMBER    0xab
#define DLN2_RES_INVALID_BUFFER_SIZE   0xae
#define DLN2_RES_I2C_MASTER_SENDING_ADDRESS_FAILED 0xba
#define DLN2_RES_I2C_MASTER_SENDING_DATA_FAILED 0xbb
#define DLN2_RES_INVALID_CHANNEL_NUMBER 0xc0
#define DLN2_RES_INVALID_MODE          0xc7
#define DLN2_RES_INVALID_VALUE         0xe2
#define DLN2_RES_SPI_MASTER_INVALID_SS_VALUE 0xb9

typedef enum {
    DLN2_HANDLE_EVENT = 0,
    DLN2_HANDLE_CTRL = 1,
    DLN2_HANDLE_GPIO = 2,
    DLN2_HANDLE_I2C = 3,
    DLN2_HANDLE_SPI = 4,
    DLN2_HANDLE_ADC = 5,
} dln2_handle_t;

typedef struct __attribute__((packed)) {
    uint16_t size;
    uint16_t id;
    uint16_t echo;
    uint16_t handle;
} dln2_header_t;

typedef struct __attribute__((packed)) {
    dln2_header_t hdr;
    uint16_t result;
} dln2_response_t;

typedef struct {
    uint16_t id;
    uint16_t echo;
    uint16_t handle;
    const uint8_t *payload;
    size_t payload_len;
} dln2_request_t;

static inline uint16_t dln2_get_le16(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t dln2_get_le32(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void dln2_put_le16(void *ptr, uint16_t value)
{
    uint8_t *p = (uint8_t *)ptr;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void dln2_put_le32(void *ptr, uint32_t value)
{
    uint8_t *p = (uint8_t *)ptr;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

size_t dln2_make_response(const dln2_request_t *request, uint16_t result,
                          const void *payload, size_t payload_len,
                          uint8_t *response, size_t response_cap);
size_t dln2_make_event(uint16_t id, uint16_t echo, const void *payload,
                       size_t payload_len, uint8_t *event, size_t event_cap);

void dln2_pin_init(void);
uint16_t dln2_pin_claim(int gpio, uint8_t module);
uint16_t dln2_pin_release(int gpio, uint8_t module);
bool dln2_pin_is_claimed(int gpio, uint8_t module);
const char *dln2_module_name(uint8_t module);

void dln2_i2c_init(void);
size_t dln2_i2c_handle_request(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap);

void dln2_spi_init(void);
size_t dln2_spi_handle_request(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap);

void dln2_adc_init(void);
size_t dln2_adc_handle_request(const dln2_request_t *request,
                               uint8_t *response, size_t response_cap);

void dln2_gpio_init(void);
size_t dln2_gpio_handle_request(const dln2_request_t *request,
                                uint8_t *response, size_t response_cap);
bool dln2_gpio_next_event(uint8_t *event, size_t event_cap, size_t *event_len,
                          uint32_t timeout_ms);
