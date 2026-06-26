#include "tool_oled.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "generated/brn_profile_config.h"

#ifndef BRN_PROFILE_OLED_I2C_SCL
#error "tool-oled requires oled.i2c_scl in the firmware Profile"
#endif
#ifndef BRN_PROFILE_OLED_I2C_SDA
#error "tool-oled requires oled.i2c_sda in the firmware Profile"
#endif

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_ADDRESS 0x3C
#define OLED_I2C_PORT I2C_NUM_1

static const char *TAG = "tool_oled";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static SemaphoreHandle_t s_lock;
static bool s_ready;

static esp_err_t write_bytes(uint8_t control, const uint8_t *data, size_t length)
{
    uint8_t buffer[17];
    if (length > sizeof(buffer) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[0] = control;
    memcpy(&buffer[1], data, length);
    return i2c_master_transmit(s_device, buffer, length + 1, 500);
}

static esp_err_t command(uint8_t value)
{
    return write_bytes(0x00, &value, 1);
}

static esp_err_t command_list(const uint8_t *commands, size_t length)
{
    esp_err_t err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < length; ++i) {
        err = command(commands[i]);
    }
    return err;
}

static esp_err_t set_cursor(uint8_t page, uint8_t column)
{
    esp_err_t err = command((uint8_t)(0xB0 | page));
    if (err == ESP_OK) err = command((uint8_t)(0x00 | (column & 0x0F)));
    if (err == ESP_OK) err = command((uint8_t)(0x10 | (column >> 4)));
    return err;
}

static esp_err_t clear_screen(void)
{
    uint8_t zeros[16] = {0};
    esp_err_t err = ESP_OK;
    for (uint8_t page = 0; err == ESP_OK && page < OLED_PAGES; ++page) {
        err = set_cursor(page, 0);
        for (uint8_t column = 0; err == ESP_OK && column < OLED_WIDTH; column += sizeof(zeros)) {
            err = write_bytes(0x40, zeros, sizeof(zeros));
        }
    }
    return err;
}

static const uint8_t *glyph(char input)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t unknown[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static const uint8_t digits[10][5] = {
        {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
        {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
        {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    };
    static const uint8_t letters[26][5] = {
        {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
        {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
        {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
        {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
        {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
        {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
        {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
        {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
        {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
        {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    };
    static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t dot[5] = {0,0x60,0x60,0,0};
    static const uint8_t colon[5] = {0,0x36,0x36,0,0};
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};

    unsigned char value = (unsigned char)input;
    if (value == ' ') return blank;
    if (value >= '0' && value <= '9') return digits[value - '0'];
    value = (unsigned char)toupper(value);
    if (value >= 'A' && value <= 'Z') return letters[value - 'A'];
    if (value == '-') return dash;
    if (value == '.') return dot;
    if (value == ':') return colon;
    if (value == '/') return slash;
    return unknown;
}

static esp_err_t draw_char(char value)
{
    uint8_t packet[6];
    memcpy(packet, glyph(value), 5);
    packet[5] = 0x00;
    return write_bytes(0x40, packet, sizeof(packet));
}

static esp_err_t draw_text(const char *text)
{
    esp_err_t err = clear_screen();
    uint8_t page = 0;
    uint8_t column = 0;
    if (err == ESP_OK) {
        err = set_cursor(page, column);
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         err == ESP_OK && *cursor != '\0' && page < OLED_PAGES;
         ++cursor) {
        if (*cursor == '\n' || column + 6 > OLED_WIDTH) {
            page++;
            column = 0;
            if (page >= OLED_PAGES) {
                break;
            }
            err = set_cursor(page, column);
            if (*cursor == '\n') {
                continue;
            }
        }
        char value = (*cursor < 0x80) ? (char)*cursor : '?';
        err = draw_char(value);
        column += 6;
        while (cursor[1] >= 0x80 && cursor[1] < 0xC0) {
            ++cursor;
        }
    }
    return err;
}

esp_err_t tool_oled_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = BRN_PROFILE_OLED_I2C_SDA,
        .scl_io_num = BRN_PROFILE_OLED_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG, "initialize OLED I2C bus");

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &device_config, &s_device), TAG, "attach OLED");
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_bus, OLED_ADDRESS, 500), TAG, "OLED not found at 0x3C");

    static const uint8_t init_sequence[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40,
        0x81, 0x7F, 0xA1, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF
    };
    ESP_RETURN_ON_ERROR(command_list(init_sequence, sizeof(init_sequence)), TAG, "initialize SSD1306");

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "SSD1306 OLED ready at I2C address 0x3C");
    return draw_text("READY");
}

esp_err_t tool_oled_execute(const char *input_json, char *output, size_t output_size)
{
    if (!input_json || !output || output_size == 0 || !s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    if (!text || text[0] == '\0') {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'text' is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: OLED is busy");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = draw_text(text);
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: OLED write failed (%s)", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "Displayed text on SSD1306 OLED");
    return ESP_OK;
}
