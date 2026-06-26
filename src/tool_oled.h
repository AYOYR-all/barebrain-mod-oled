#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t tool_oled_init(void);
esp_err_t tool_oled_execute(const char *input_json, char *output, size_t output_size);
