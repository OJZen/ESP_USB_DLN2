#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define C3_TEST_VERSION "c3-gpio-probe-v1"
#define MAX_LINE_LEN 128

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
           " read=0x%02" PRIx32 "\n",
           s_output_enable_mask, s_output_value_mask, read_mask());
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
        esp_err_t err = set_inputs(ALL_CHANNELS_MASK);
        if (err != ESP_OK) {
            printf("ERR RESET %s\n", esp_err_to_name(err));
            return;
        }
        s_output_value_mask = 0;
        printf("OK RESET\n");
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
        printf("OK HELP PING MAP STATUS READ RESET IN <mask> DRIVE <mask> <value> PULSE <mask> <count> <high_ms> <low_ms>\n");
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
