#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

esp_err_t tool_oled_init(void);
esp_err_t tool_oled_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_oled_set_clock_enabled(bool enabled);
esp_err_t tool_oled_draw_clock(void);
esp_err_t tool_oled_draw_face(const char *emotion, unsigned int frame);
