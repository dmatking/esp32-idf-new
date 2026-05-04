# CYD-S3 2.8" ILI9341 Capacitive Touch — `cyd28s3_ili9341_touch`

LCDWIKI ES3C28P — 2.8" IPS 240×320 display with ILI9341V driver IC,
FT6336G capacitive touch, WS2812B RGB LED, MicroSD slot, audio (ES8311 codec +
FM8002E amplifier + MEMS mic), and battery management on an ESP32-S3R8 module
(8MB OPI PSRAM, 16MB Flash, USB-C Type-C interface with no separate UART IC).

This board file initialises the display, touch, and RGB LED only. Audio, SD, and
battery circuits are present on the hardware but not managed here.

## Pin Assignment

### LCD (ILI9341V) — SPI2 host (FSPI)

| Signal | GPIO |
|--------|------|
| SCLK   | 12   |
| MOSI   | 11   |
| MISO   | 13   |
| DC     | 46   |
| CS     | 10   |
| RST    | —    |
| BL     | 45 (high = on) |

### Touch (FT6336G) — I2C0, address 0x38

| Signal | GPIO |
|--------|------|
| SCL    | 15   |
| SDA    | 16   |
| INT    | 17 (active low) |
| RST    | 18 (active low) |

The I2C bus is shared with the ES8311 audio codec (address 0x18).
Board has 4.7 kΩ hardware pull-ups on SDA and SCL.

### WS2812B RGB LED

| Signal | GPIO |
|--------|------|
| DATA   | 42   |

Single addressable pixel. Driven via RMT peripheral using the
`espressif/led_strip` component. Not a common-anode LED.

## Notes

- Display is IPS (vs TN on the original CYD); colours are correct from all
  viewing angles and no inversion quirk is needed.
- Touch uses `espressif/esp_lcd_touch_ft5x06`; the FT6336G is protocol-compatible
  with the FT5x06 driver family.
- `tp_io_cfg.scl_speed_hz = 0` is required when using the legacy `driver/i2c.h`
  API — the legacy driver ignores the speed field and setting it non-zero triggers
  `ESP_ERR_INVALID_ARG` inside the touch component.
- Console output goes via USB_SERIAL_JTAG (the ESP32-S3's built-in USB); there is
  no separate USB-to-UART IC on this board.
- The `espressif/esp_lcd_ili9341`, `espressif/esp_lcd_touch_ft5x06`, and
  `espressif/led_strip` components are fetched from the IDF Component Registry on
  first build.
