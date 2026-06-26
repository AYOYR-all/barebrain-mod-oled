#include "tool_oled.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
#define OLED_I2C_PORT I2C_NUM_1

static const char *TAG = "tool_oled";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static uint8_t s_address;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static bool s_clock_enabled = true;
static uint8_t s_framebuffer[OLED_WIDTH * OLED_PAGES];

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

static void fb_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

static void fb_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    uint16_t index = (uint16_t)(x + (y / 8) * OLED_WIDTH);
    uint8_t bit = (uint8_t)(1U << (y % 8));
    if (on) {
        s_framebuffer[index] |= bit;
    } else {
        s_framebuffer[index] &= (uint8_t)~bit;
    }
}

static void fb_fill_rect(int x, int y, int width, int height, bool on)
{
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) {
            fb_set_pixel(px, py, on);
        }
    }
}

static void fb_draw_hline(int x, int y, int width, bool on)
{
    for (int px = x; px < x + width; ++px) {
        fb_set_pixel(px, y, on);
    }
}

static void fb_draw_vline(int x, int y, int height, bool on)
{
    for (int py = y; py < y + height; ++py) {
        fb_set_pixel(x, py, on);
    }
}

static void fb_draw_rect(int x, int y, int width, int height, bool on)
{
    fb_draw_hline(x, y, width, on);
    fb_draw_hline(x, y + height - 1, width, on);
    fb_draw_vline(x, y, height, on);
    fb_draw_vline(x + width - 1, y, height, on);
}

static esp_err_t fb_flush(void)
{
    esp_err_t err = ESP_OK;
    for (uint8_t page = 0; err == ESP_OK && page < OLED_PAGES; ++page) {
        err = set_cursor(page, 0);
        for (uint8_t column = 0; err == ESP_OK && column < OLED_WIDTH; column += 16) {
            err = write_bytes(0x40, &s_framebuffer[page * OLED_WIDTH + column], 16);
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

static void fb_draw_char(int x, int y, char value, int scale)
{
    const uint8_t *bitmap = glyph(value);
    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 8; ++row) {
            if (!(bitmap[column] & (1U << row))) {
                continue;
            }
            for (int dx = 0; dx < scale; ++dx) {
                for (int dy = 0; dy < scale; ++dy) {
                    fb_set_pixel(x + column * scale + dx, y + row * scale + dy, true);
                }
            }
        }
    }
}

static void fb_draw_text(int x, int y, const char *text, int scale)
{
    int cursor_x = x;
    int step = 6 * scale;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        fb_draw_char(cursor_x, y, *cursor, scale);
        cursor_x += step;
    }
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

static esp_err_t draw_clock_screen(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    char time_text[6];
    char date_text[11];
    int hour;
    int minute;
    int year;
    int month;
    int day;
    int weekday;
    static const char *const weekdays[] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };

    time(&now);
    localtime_r(&now, &timeinfo);
    hour = (timeinfo.tm_hour >= 0 && timeinfo.tm_hour <= 23) ? timeinfo.tm_hour : 0;
    minute = (timeinfo.tm_min >= 0 && timeinfo.tm_min <= 59) ? timeinfo.tm_min : 0;
    year = timeinfo.tm_year + 1900;
    year = (year >= 1970 && year <= 9999) ? year : 1970;
    month = (timeinfo.tm_mon >= 0 && timeinfo.tm_mon <= 11) ? timeinfo.tm_mon + 1 : 1;
    day = (timeinfo.tm_mday >= 1 && timeinfo.tm_mday <= 31) ? timeinfo.tm_mday : 1;
    weekday = (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) ? timeinfo.tm_wday : 0;

    snprintf(time_text, sizeof(time_text), "%02d:%02d", hour, minute);
    snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d", year, month, day);

    fb_clear();
    fb_draw_text(19, 4, time_text, 3);
    fb_draw_text(4, 42, date_text, 1);
    fb_draw_text(86, 38, weekdays[weekday], 2);
    return fb_flush();
}

static bool emotion_is(const char *emotion, const char *expected)
{
    return emotion && strcasecmp(emotion, expected) == 0;
}

static void fb_draw_eye_pair(int left_y, int right_y, int height)
{
    fb_fill_rect(25, left_y, 26, height, true);
    fb_fill_rect(77, right_y, 26, height, true);
}

static void fb_draw_mouth_idle(void)
{
    fb_draw_hline(52, 50, 24, true);
}

static void fb_draw_mouth_happy(void)
{
    fb_set_pixel(48, 45, true);
    fb_set_pixel(49, 46, true);
    fb_set_pixel(50, 47, true);
    fb_draw_hline(51, 48, 26, true);
    fb_set_pixel(77, 47, true);
    fb_set_pixel(78, 46, true);
    fb_set_pixel(79, 45, true);
}

static void fb_draw_mouth_error(void)
{
    fb_draw_hline(48, 53, 32, true);
    fb_draw_hline(49, 52, 30, true);
}

static esp_err_t draw_face_screen(const char *emotion, unsigned int frame)
{
    fb_clear();

    if (emotion_is(emotion, "happy")) {
        fb_draw_hline(25, 30, 26, true);
        fb_draw_hline(77, 30, 26, true);
        fb_set_pixel(24, 31, true);
        fb_set_pixel(51, 31, true);
        fb_set_pixel(76, 31, true);
        fb_set_pixel(103, 31, true);
        fb_draw_mouth_happy();
    } else if (emotion_is(emotion, "thinking")) {
        fb_draw_eye_pair(24, 20, 12);
        fb_draw_text(92, 44, "...", 1);
        fb_draw_hline(50, 51, 26, true);
        if ((frame % 2U) == 0U) {
            fb_draw_rect(18, 18, 38, 24, true);
        }
    } else if (emotion_is(emotion, "error")) {
        for (int i = 0; i < 24; ++i) {
            fb_set_pixel(25 + i, 20 + i, true);
            fb_set_pixel(48 - i, 20 + i, true);
            fb_set_pixel(79 + i, 20 + i, true);
            fb_set_pixel(102 - i, 20 + i, true);
        }
        fb_draw_mouth_error();
    } else if (emotion_is(emotion, "speaking")) {
        fb_draw_eye_pair(20, 20, 18);
        if ((frame % 2U) == 0U) {
            fb_draw_rect(52, 48, 24, 8, true);
        } else {
            fb_fill_rect(52, 45, 24, 14, true);
            fb_fill_rect(56, 49, 16, 6, false);
        }
    } else {
        int blink = ((frame % 12U) == 0U) ? 4 : 18;
        int eye_y = ((frame % 12U) == 0U) ? 27 : 20;
        fb_draw_eye_pair(eye_y, eye_y, blink);
        fb_draw_mouth_idle();
    }

    return fb_flush();
}

static void clock_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_ready && s_clock_enabled && xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            esp_err_t err = draw_clock_screen();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "clock refresh failed: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

esp_err_t tool_oled_set_clock_enabled(bool enabled)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_clock_enabled = enabled;
    return ESP_OK;
}

esp_err_t tool_oled_draw_clock(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = draw_clock_screen();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t tool_oled_draw_face(const char *emotion, unsigned int frame)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = draw_face_screen(emotion ? emotion : "idle", frame);
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t attach_oled_device(void)
{
    static const uint8_t candidates[] = {0x3C, 0x3D};
    esp_err_t last_err = ESP_ERR_NOT_FOUND;

    for (size_t i = 0; i < sizeof(candidates); ++i) {
        uint8_t address = candidates[i];
        esp_err_t err = i2c_master_probe(s_bus, address, 500);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OLED not found at I2C address 0x%02X: %s", address, esp_err_to_name(err));
            last_err = err;
            continue;
        }

        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = 400000,
        };
        err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
        if (err == ESP_OK) {
            s_address = address;
            ESP_LOGI(TAG, "OLED found at I2C address 0x%02X", address);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "OLED address 0x%02X responded but attach failed: %s", address, esp_err_to_name(err));
        last_err = err;
    }

    ESP_LOGE(TAG, "OLED was not found at 0x3C or 0x3D. Check VCC/GND/SCL/SDA and Manager GPIO config.");
    return last_err;
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
    ESP_RETURN_ON_ERROR(attach_oled_device(), TAG, "attach OLED");

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
    ESP_LOGI(TAG, "SSD1306 OLED ready at I2C address 0x%02X", s_address);
    ESP_RETURN_ON_ERROR(draw_clock_screen(), TAG, "draw clock screen");
    BaseType_t created = xTaskCreate(clock_task, "tool_oled_clock", 3072, NULL, 3, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
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
