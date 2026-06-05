#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_slave.h"
#include "driver/spi_slave.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define C3_TEST_VERSION "c3-probe-v4"
#define MAX_LINE_LEN 128
#define I2C_TEST_PORT I2C_NUM_0
#define I2C_TEST_SDA_GPIO GPIO_NUM_0
#define I2C_TEST_SCL_GPIO GPIO_NUM_1
#define I2C_TEST_DEFAULT_ADDR 0x52
#define I2C_TEST_MAX_BYTES 256
#define I2C_TEST_TX_DEPTH 512
#define SPI_TEST_HOST SPI2_HOST
#define SPI_TEST_SCLK_GPIO GPIO_NUM_4
#define SPI_TEST_MOSI_GPIO GPIO_NUM_5
#define SPI_TEST_MISO_GPIO GPIO_NUM_6
#define SPI_TEST_CS_GPIO GPIO_NUM_7
#define SPI_TEST_MAX_BYTES 256
#define SPI_TEST_DEFAULT_TIMEOUT_MS 5000

static const gpio_num_t s_channels[] = {
    GPIO_NUM_0,
    GPIO_NUM_1,
    GPIO_NUM_3,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7,
    GPIO_NUM_10,
};

#define CHANNEL_COUNT ((uint32_t)(sizeof(s_channels) / sizeof(s_channels[0])))
#define ALL_CHANNELS_MASK ((1UL << CHANNEL_COUNT) - 1UL)

static uint32_t s_output_enable_mask;
static uint32_t s_output_value_mask;
static bool s_i2c_active;
static uint8_t s_i2c_addr;
static volatile bool s_i2c_rx_done;
static volatile uint32_t s_i2c_rx_armed_len;
static volatile uint32_t s_i2c_last_rx_len;
static uint32_t s_i2c_rx_count;
static uint32_t s_i2c_tx_count;
static uint32_t s_i2c_tx_len;
static i2c_slave_dev_handle_t s_i2c_handle;
static uint8_t s_i2c_rx_buf[I2C_TEST_MAX_BYTES];
static uint8_t s_i2c_last_rx[I2C_TEST_MAX_BYTES];
static uint8_t s_i2c_tx_buf[I2C_TEST_MAX_BYTES];
static bool s_spi_active;
static uint8_t s_spi_mode;
static uint32_t s_spi_xfer_count;
static uint32_t s_spi_last_len;
DMA_ATTR static uint8_t s_spi_last_rx[SPI_TEST_MAX_BYTES];
DMA_ATTR static uint8_t s_spi_last_tx[SPI_TEST_MAX_BYTES];
DMA_ATTR static uint8_t s_spi_rx_buf[SPI_TEST_MAX_BYTES];
DMA_ATTR static uint8_t s_spi_tx_buf[SPI_TEST_MAX_BYTES];

static esp_err_t set_inputs(uint32_t mask);
static esp_err_t i2c_test_stop(void);
static esp_err_t spi_test_stop(void);

static esp_err_t configure_console_io(void)
{
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    fflush(stdout);
    fsync(fileno(stdout));

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };

    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    usb_serial_jtag_vfs_use_driver();
    fcntl(fileno(stdin), F_SETFL, 0);
    fcntl(fileno(stdout), F_SETFL, 0);
#endif
    return ESP_OK;
}

static char *next_token(char **cursor)
{
    char *start = *cursor;

    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (!*start) {
        *cursor = start;
        return NULL;
    }

    char *end = start;
    while (*end && !isspace((unsigned char)*end)) {
        end++;
    }

    if (*end) {
        *end++ = '\0';
    }

    *cursor = end;
    return start;
}

static bool parse_u32(char **cursor, uint32_t *value)
{
    char *token = next_token(cursor);
    char *end = NULL;

    if (!token) {
        return false;
    }

    unsigned long parsed = strtoul(token, &end, 0);
    if (!end || *end != '\0') {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static uint32_t sanitize_mask(uint32_t mask)
{
    return mask & ALL_CHANNELS_MASK;
}

static void spi_test_fill_tx(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        s_spi_tx_buf[i] = (uint8_t)(0xa0U + (i & 0x3fU));
    }
}

static void i2c_test_fill_tx(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        s_i2c_tx_buf[i] = (uint8_t)(0xc0U + (i & 0x3fU));
    }
}

static void print_hex(const uint8_t *buf, uint32_t len)
{
    static const char hex[] = "0123456789abcdef";

    for (uint32_t i = 0; i < len; i++) {
        putchar(hex[buf[i] >> 4]);
        putchar(hex[buf[i] & 0x0f]);
    }
}

static bool i2c_test_rx_done_cb(i2c_slave_dev_handle_t i2c_slave,
                                const i2c_slave_rx_done_event_data_t *edata,
                                void *arg)
{
    (void)i2c_slave;
    (void)edata;
    (void)arg;

    uint32_t len = s_i2c_rx_armed_len;
    if (len > I2C_TEST_MAX_BYTES) {
        len = I2C_TEST_MAX_BYTES;
    }

    memcpy(s_i2c_last_rx, s_i2c_rx_buf, len);
    s_i2c_last_rx_len = len;
    s_i2c_rx_armed_len = 0;
    s_i2c_rx_count++;
    s_i2c_rx_done = true;
    return false;
}

static esp_err_t i2c_test_stop(void)
{
    if (!s_i2c_active) {
        return ESP_OK;
    }

    esp_err_t err = i2c_del_slave_device(s_i2c_handle);
    if (err == ESP_OK) {
        s_i2c_active = false;
        s_i2c_handle = NULL;
        s_i2c_rx_armed_len = 0;
        s_i2c_rx_done = false;
        set_inputs(ALL_CHANNELS_MASK);
    }
    return err;
}

static esp_err_t i2c_test_start(uint32_t addr)
{
    if (addr == 0 || addr > 0x7f) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_i2c_active) {
        if (s_i2c_addr == addr) {
            return ESP_OK;
        }

        esp_err_t err = i2c_test_stop();
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = spi_test_stop();
    if (err != ESP_OK) {
        return err;
    }

    err = set_inputs(ALL_CHANNELS_MASK);
    if (err != ESP_OK) {
        return err;
    }

    i2c_slave_config_t config = {
        .i2c_port = I2C_TEST_PORT,
        .sda_io_num = I2C_TEST_SDA_GPIO,
        .scl_io_num = I2C_TEST_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = I2C_TEST_TX_DEPTH,
        .slave_addr = (uint16_t)addr,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
    };

    err = i2c_new_slave_device(&config, &s_i2c_handle);
    if (err != ESP_OK) {
        return err;
    }

    i2c_slave_event_callbacks_t cbs = {
        .on_recv_done = i2c_test_rx_done_cb,
    };

    err = i2c_slave_register_event_callbacks(s_i2c_handle, &cbs, NULL);
    if (err != ESP_OK) {
        i2c_del_slave_device(s_i2c_handle);
        s_i2c_handle = NULL;
        return err;
    }

    s_i2c_active = true;
    s_i2c_addr = (uint8_t)addr;
    s_i2c_rx_done = false;
    s_i2c_rx_armed_len = 0;
    s_i2c_last_rx_len = 0;
    s_i2c_rx_count = 0;
    s_i2c_tx_count = 0;
    s_i2c_tx_len = 0;
    return ESP_OK;
}

static esp_err_t i2c_test_arm_rx(uint32_t len)
{
    if (!s_i2c_active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len == 0 || len > I2C_TEST_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(s_i2c_rx_buf, 0, sizeof(s_i2c_rx_buf));
    s_i2c_rx_done = false;
    s_i2c_rx_armed_len = len;
    s_i2c_last_rx_len = 0;

    esp_err_t err = i2c_slave_receive(s_i2c_handle, s_i2c_rx_buf, len);
    if (err != ESP_OK) {
        s_i2c_rx_armed_len = 0;
    }
    return err;
}

static esp_err_t i2c_test_load_tx(uint32_t len)
{
    if (!s_i2c_active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len == 0 || len > I2C_TEST_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    i2c_test_fill_tx(len);
    esp_err_t err = i2c_slave_transmit(s_i2c_handle, s_i2c_tx_buf, len, 1000);
    if (err == ESP_OK) {
        s_i2c_tx_len = len;
        s_i2c_tx_count++;
    }
    return err;
}

static esp_err_t spi_test_stop(void)
{
    if (!s_spi_active) {
        return ESP_OK;
    }

    esp_err_t err = spi_slave_free(SPI_TEST_HOST);
    if (err == ESP_OK) {
        s_spi_active = false;
        set_inputs(ALL_CHANNELS_MASK);
    }
    return err;
}

static esp_err_t spi_test_start(uint32_t mode)
{
    if (mode > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_spi_active) {
        if (s_spi_mode == mode) {
            return ESP_OK;
        }

        esp_err_t err = spi_test_stop();
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = i2c_test_stop();
    if (err != ESP_OK) {
        return err;
    }

    err = set_inputs(ALL_CHANNELS_MASK);
    if (err != ESP_OK) {
        return err;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_TEST_MOSI_GPIO,
        .miso_io_num = SPI_TEST_MISO_GPIO,
        .sclk_io_num = SPI_TEST_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_TEST_MAX_BYTES,
    };
    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = SPI_TEST_CS_GPIO,
        .flags = 0,
        .queue_size = 1,
        .mode = (uint8_t)mode,
    };

    gpio_set_pull_mode(SPI_TEST_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SPI_TEST_SCLK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SPI_TEST_CS_GPIO, GPIO_PULLUP_ONLY);

    err = spi_slave_initialize(SPI_TEST_HOST, &buscfg, &slvcfg,
                               SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        s_spi_active = true;
        s_spi_mode = (uint8_t)mode;
        s_spi_last_len = 0;
        s_spi_xfer_count = 0;
    }
    return err;
}

static esp_err_t spi_test_xfer(uint32_t len, uint32_t timeout_ms)
{
    if (!s_spi_active) {
        esp_err_t err = spi_test_start(s_spi_mode);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (len == 0 || len > SPI_TEST_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(s_spi_rx_buf, 0, sizeof(s_spi_rx_buf));
    memset(s_spi_tx_buf, 0, sizeof(s_spi_tx_buf));
    spi_test_fill_tx(len);

    spi_slave_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = s_spi_tx_buf,
        .rx_buffer = s_spi_rx_buf,
    };

    esp_err_t err = spi_slave_transmit(SPI_TEST_HOST, &trans,
                                       pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t actual_len = (uint32_t)((trans.trans_len + 7) / 8);
    if (actual_len > SPI_TEST_MAX_BYTES) {
        actual_len = SPI_TEST_MAX_BYTES;
    }

    memcpy(s_spi_last_rx, s_spi_rx_buf, actual_len);
    memcpy(s_spi_last_tx, s_spi_tx_buf, actual_len);
    s_spi_last_len = actual_len;
    s_spi_xfer_count++;
    return ESP_OK;
}

static esp_err_t channel_input(uint32_t channel)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_channels[channel],
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        s_output_enable_mask &= ~(1UL << channel);
    }
    return err;
}

static esp_err_t channel_output(uint32_t channel, bool value)
{
    gpio_num_t gpio = s_channels[channel];
    gpio_set_level(gpio, value ? 1 : 0);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        gpio_set_level(gpio, value ? 1 : 0);
        s_output_enable_mask |= 1UL << channel;
        if (value) {
            s_output_value_mask |= 1UL << channel;
        } else {
            s_output_value_mask &= ~(1UL << channel);
        }
    }
    return err;
}

static esp_err_t set_inputs(uint32_t mask)
{
    mask = sanitize_mask(mask);
    for (uint32_t channel = 0; channel < CHANNEL_COUNT; channel++) {
        if (mask & (1UL << channel)) {
            esp_err_t err = channel_input(channel);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t drive_mask(uint32_t mask, uint32_t value_mask)
{
    mask = sanitize_mask(mask);
    value_mask = sanitize_mask(value_mask);

    for (uint32_t channel = 0; channel < CHANNEL_COUNT; channel++) {
        if (mask & (1UL << channel)) {
            esp_err_t err = channel_output(channel, value_mask & (1UL << channel));
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static uint32_t read_mask(void)
{
    uint32_t value = 0;

    for (uint32_t channel = 0; channel < CHANNEL_COUNT; channel++) {
        if (gpio_get_level(s_channels[channel])) {
            value |= 1UL << channel;
        }
    }

    return value;
}

static void print_map(void)
{
    printf("OK MAP");
    for (uint32_t channel = 0; channel < CHANNEL_COUNT; channel++) {
        printf(" %" PRIu32 ":GPIO%d", channel, (int)s_channels[channel]);
    }
    printf("\n");
}

static void print_status(void)
{
    printf("OK STATUS mode=0x%02" PRIx32 " out=0x%02" PRIx32
           " read=0x%02" PRIx32 " spi=%s spi_count=%" PRIu32
           " spi_len=%" PRIu32 " spi_mode=%u i2c=%s i2c_addr=0x%02x\n",
           s_output_enable_mask, s_output_value_mask, read_mask(),
           s_spi_active ? "on" : "off", s_spi_xfer_count, s_spi_last_len,
           s_spi_mode, s_i2c_active ? "on" : "off", s_i2c_addr);
}

static void print_spi_status(void)
{
    printf("OK SPI STATUS active=%" PRIu32 " mode=%u count=%" PRIu32
           " len=%" PRIu32 " rx=",
           (uint32_t)(s_spi_active ? 1 : 0), s_spi_mode, s_spi_xfer_count,
           s_spi_last_len);
    print_hex(s_spi_last_rx, s_spi_last_len);
    printf(" tx=");
    print_hex(s_spi_last_tx, s_spi_last_len);
    printf("\n");
}

static void print_i2c_status(void)
{
    printf("OK I2C STATUS active=%" PRIu32 " addr=0x%02x rx_done=%" PRIu32
           " rx_armed=%" PRIu32 " rx_count=%" PRIu32 " tx_count=%" PRIu32
           " rx_len=%" PRIu32 " tx_len=%" PRIu32 " rx=",
           (uint32_t)(s_i2c_active ? 1 : 0), s_i2c_addr,
           (uint32_t)(s_i2c_rx_done ? 1 : 0), (uint32_t)s_i2c_rx_armed_len,
           s_i2c_rx_count, s_i2c_tx_count, (uint32_t)s_i2c_last_rx_len,
           s_i2c_tx_len);
    print_hex(s_i2c_last_rx, s_i2c_last_rx_len);
    printf(" tx=");
    print_hex(s_i2c_tx_buf, s_i2c_tx_len);
    printf("\n");
}

static void process_line(char *line)
{
    char *cursor = line;
    char *cmd = next_token(&cursor);

    if (!cmd) {
        return;
    }

    for (char *p = cmd; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }

    if (strcmp(cmd, "PING") == 0) {
        printf("OK PONG %s channels=%" PRIu32 "\n", C3_TEST_VERSION,
               CHANNEL_COUNT);
        return;
    }

    if (strcmp(cmd, "MAP") == 0) {
        print_map();
        return;
    }

    if (strcmp(cmd, "STATUS") == 0) {
        print_status();
        return;
    }

    if (strcmp(cmd, "READ") == 0) {
        printf("OK READ 0x%02" PRIx32 "\n", read_mask());
        return;
    }

    if (strcmp(cmd, "RESET") == 0) {
        esp_err_t err = spi_test_stop();
        if (err == ESP_OK) {
            err = i2c_test_stop();
        }
        if (err == ESP_OK) {
            err = set_inputs(ALL_CHANNELS_MASK);
        }
        if (err != ESP_OK) {
            printf("ERR RESET %s\n", esp_err_to_name(err));
            return;
        }
        s_output_value_mask = 0;
        printf("OK RESET\n");
        return;
    }

    if (strcmp(cmd, "I2C") == 0) {
        char *subcmd = next_token(&cursor);
        if (!subcmd) {
            printf("ERR I2C expected-subcommand\n");
            return;
        }

        for (char *p = subcmd; *p; p++) {
            *p = (char)toupper((unsigned char)*p);
        }

        if (strcmp(subcmd, "START") == 0) {
            uint32_t addr = I2C_TEST_DEFAULT_ADDR;
            parse_u32(&cursor, &addr);

            esp_err_t err = i2c_test_start(addr);
            if (err != ESP_OK) {
                printf("ERR I2C START %s\n", esp_err_to_name(err));
                return;
            }
            printf("OK I2C START addr=0x%02x sda=%d scl=%d max=%d\n",
                   s_i2c_addr, (int)I2C_TEST_SDA_GPIO,
                   (int)I2C_TEST_SCL_GPIO, I2C_TEST_MAX_BYTES);
            return;
        }

        if (strcmp(subcmd, "STOP") == 0) {
            esp_err_t err = i2c_test_stop();
            if (err != ESP_OK) {
                printf("ERR I2C STOP %s\n", esp_err_to_name(err));
                return;
            }
            printf("OK I2C STOP\n");
            return;
        }

        if (strcmp(subcmd, "STATUS") == 0) {
            print_i2c_status();
            return;
        }

        if (strcmp(subcmd, "RX") == 0) {
            uint32_t len;
            if (!parse_u32(&cursor, &len)) {
                printf("ERR I2C RX expected-len\n");
                return;
            }

            esp_err_t err = i2c_test_arm_rx(len);
            if (err != ESP_OK) {
                printf("ERR I2C RX %s\n", esp_err_to_name(err));
                return;
            }
            printf("OK I2C RX len=%" PRIu32 "\n", len);
            return;
        }

        if (strcmp(subcmd, "LOAD") == 0) {
            uint32_t len;
            if (!parse_u32(&cursor, &len)) {
                printf("ERR I2C LOAD expected-len\n");
                return;
            }

            esp_err_t err = i2c_test_load_tx(len);
            if (err != ESP_OK) {
                printf("ERR I2C LOAD %s\n", esp_err_to_name(err));
                return;
            }
            printf("OK I2C LOAD len=%" PRIu32 " tx=", len);
            print_hex(s_i2c_tx_buf, len);
            printf("\n");
            return;
        }

        printf("ERR I2C unknown-subcommand\n");
        return;
    }

    if (strcmp(cmd, "SPI") == 0) {
        char *subcmd = next_token(&cursor);
        if (!subcmd) {
            printf("ERR SPI expected-subcommand\n");
            return;
        }

        for (char *p = subcmd; *p; p++) {
            *p = (char)toupper((unsigned char)*p);
        }

        if (strcmp(subcmd, "START") == 0) {
            uint32_t mode = 0;
            parse_u32(&cursor, &mode);

            esp_err_t err = spi_test_start(mode);
            if (err != ESP_OK) {
                printf("ERR SPI START %s\n", esp_err_to_name(err));
                return;
            }
            printf("OK SPI START mode=%u sclk=%d mosi=%d miso=%d cs=%d max=%d\n",
                   s_spi_mode,
                   (int)SPI_TEST_SCLK_GPIO, (int)SPI_TEST_MOSI_GPIO,
                   (int)SPI_TEST_MISO_GPIO, (int)SPI_TEST_CS_GPIO,
                   SPI_TEST_MAX_BYTES);
            return;
        }

        if (strcmp(subcmd, "STOP") == 0) {
            esp_err_t err = spi_test_stop();
            if (err != ESP_OK) {
                printf("ERR SPI STOP %s\n", esp_err_to_name(err));
                return;
            }
            printf("OK SPI STOP\n");
            return;
        }

        if (strcmp(subcmd, "STATUS") == 0) {
            print_spi_status();
            return;
        }

        if (strcmp(subcmd, "XFER") == 0) {
            uint32_t len;
            uint32_t timeout_ms = SPI_TEST_DEFAULT_TIMEOUT_MS;
            if (!parse_u32(&cursor, &len)) {
                printf("ERR SPI XFER expected-len\n");
                return;
            }
            parse_u32(&cursor, &timeout_ms);

            esp_err_t err = spi_test_xfer(len, timeout_ms);
            if (err != ESP_OK) {
                printf("ERR SPI XFER %s\n", esp_err_to_name(err));
                return;
            }

            printf("OK SPI XFER count=%" PRIu32 " len=%" PRIu32 " rx=",
                   s_spi_xfer_count, s_spi_last_len);
            print_hex(s_spi_last_rx, s_spi_last_len);
            printf(" tx=");
            print_hex(s_spi_last_tx, s_spi_last_len);
            printf("\n");
            return;
        }

        printf("ERR SPI unknown-subcommand\n");
        return;
    }

    if (strcmp(cmd, "IN") == 0) {
        uint32_t mask;
        if (!parse_u32(&cursor, &mask)) {
            printf("ERR IN expected-mask\n");
            return;
        }
        esp_err_t err = set_inputs(mask);
        if (err != ESP_OK) {
            printf("ERR IN %s\n", esp_err_to_name(err));
            return;
        }
        print_status();
        return;
    }

    if (strcmp(cmd, "DRIVE") == 0) {
        uint32_t mask;
        uint32_t value_mask;
        if (!parse_u32(&cursor, &mask) || !parse_u32(&cursor, &value_mask)) {
            printf("ERR DRIVE expected-mask-value\n");
            return;
        }
        esp_err_t err = drive_mask(mask, value_mask);
        if (err != ESP_OK) {
            printf("ERR DRIVE %s\n", esp_err_to_name(err));
            return;
        }
        print_status();
        return;
    }

    if (strcmp(cmd, "PULSE") == 0) {
        uint32_t mask;
        uint32_t count;
        uint32_t high_ms;
        uint32_t low_ms;
        if (!parse_u32(&cursor, &mask) || !parse_u32(&cursor, &count) ||
            !parse_u32(&cursor, &high_ms) || !parse_u32(&cursor, &low_ms)) {
            printf("ERR PULSE expected-mask-count-high_ms-low_ms\n");
            return;
        }

        mask = sanitize_mask(mask);
        esp_err_t err = drive_mask(mask, 0);
        if (err != ESP_OK) {
            printf("ERR PULSE %s\n", esp_err_to_name(err));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(low_ms));

        for (uint32_t i = 0; i < count; i++) {
            err = drive_mask(mask, mask);
            if (err != ESP_OK) {
                printf("ERR PULSE %s\n", esp_err_to_name(err));
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(high_ms));

            err = drive_mask(mask, 0);
            if (err != ESP_OK) {
                printf("ERR PULSE %s\n", esp_err_to_name(err));
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(low_ms));
        }

        set_inputs(mask);
        printf("OK PULSE mask=0x%02" PRIx32 " count=%" PRIu32 "\n", mask,
               count);
        return;
    }

    if (strcmp(cmd, "HELP") == 0) {
        printf("OK HELP PING MAP STATUS READ RESET IN <mask> DRIVE <mask> <value> PULSE <mask> <count> <high_ms> <low_ms> I2C START [addr]|STOP|STATUS|RX <len>|LOAD <len> SPI START [mode]|STOP|STATUS|XFER <len> [timeout_ms]\n");
        return;
    }

    printf("ERR unknown-command\n");
}

void app_main(void)
{
    char line[MAX_LINE_LEN];

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_err_t err = configure_console_io();
    if (err != ESP_OK) {
        printf("ERR CONSOLE %s\n", esp_err_to_name(err));
    }

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    err = set_inputs(ALL_CHANNELS_MASK);
    if (err != ESP_OK) {
        printf("ERR BOOT %s\n", esp_err_to_name(err));
    }

    printf("READY %s channels=%" PRIu32 "\n", C3_TEST_VERSION, CHANNEL_COUNT);
    print_map();

    while (fgets(line, sizeof(line), stdin)) {
        process_line(line);
    }
}
