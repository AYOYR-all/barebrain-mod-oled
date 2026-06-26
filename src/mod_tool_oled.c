#include "core/mod/brn_mod.h"
#include "tool_oled.h"
#include "tools/tool_registry.h"

static const char *const tool_oled_deps[] = {
    "core.tool_registry",
    NULL,
};

static const brn_tool_t oled_text_tool = {
    .name = "oled_text",
    .description = "Display ASCII text on the configured SSD1306 I2C OLED screen.",
    .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to display\"}},"
        "\"required\":[\"text\"]}",
    .execute = tool_oled_execute,
};

static esp_err_t tool_oled_mod_init(void)
{
    esp_err_t err = tool_oled_init();
    if (err != ESP_OK) {
        return err;
    }
    return brn_tool_register(&oled_text_tool);
}

const brn_mod_t brn_mod_tool_oled = {
    .id = "tool-oled",
    .name = "OLED Tool",
    .version = "0.1.1",
    .deps = tool_oled_deps,
    .init = tool_oled_mod_init,
};
