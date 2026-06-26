# BareBrain OLED 显示屏插件

`tool-oled` 用于驱动常见 4 针 I2C OLED 屏幕，模块引脚通常为：

- `GND`
- `VCC`
- `SCL`
- `SDA`

第一版按 SSD1306 兼容屏幕实现，I2C 地址固定为 `0x3C`。安装并启用插件后，在 BareBrain-Manager 的“引脚配置”页选择 `SCL` 和 `SDA` 两个 GPIO，云端构建生成的固件启动后会在 OLED 上显示时间、日期和星期。

## Profile 配置

```json
{
  "config": {
    "oled.i2c_scl": 5,
    "oled.i2c_sda": 6
  }
}
```

## Release 包要求

发布包 `tool-oled.zip` 的根目录必须直接包含：

- `barebrain.mod.json`
- `CMakeLists.txt`
- `src/`
