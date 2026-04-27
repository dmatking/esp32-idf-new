# CYD 2.8" ILI9341 Touch — `cyd28_ili9341_touch`

LCDWIKI E32R28T / E32N28T — 2.8" 240×320 TN display with ILI9341 driver IC,
XPT2046 resistive touch, RGB LED, microSD slot, and audio amplifier on an
ESP32-WROOM-32E module.

## Pin Assignment

### LCD (ILI9341) — SPI2 host

| Signal | GPIO |
|--------|------|
| SCLK   | 14   |
| MOSI   | 13   |
| MISO   | 12   |
| DC     | 2    |
| CS     | 15   |
| RST    | —    |
| BL     | 21 (high = on) |

### Touch (XPT2046) — SPI3 host

| Signal | GPIO |
|--------|------|
| CLK    | 25   |
| MOSI   | 32   |
| MISO   | 39 (input-only) |
| CS     | 33   |
| IRQ    | 36 (input-only, low = touched) |

### RGB LED (common anode, low = on)

| Colour | GPIO |
|--------|------|
| Red    | 17   |
| Green  | 22   |
| Blue   | 16   |

### Miscellaneous

| Function       | GPIO |
|----------------|------|
| Audio DAC out  | 26   |
| Audio enable   | 4 (low = amp on) |
| Battery ADC    | 34 (input-only) |
| LDR            | 35 (input-only) |

## Touch Calibration

`board_impl.c` logs raw XPT2046 ADC values (0–4095).  Typical active range is
~200–3800 on both axes.  To map to screen coordinates, record corner values
with `esp32-serial` and adjust the linear mapping in `touch_logger_task`.

## Notes

- ILI9341 uses BGR physical connections; `LCD_RGB_ELEMENT_ORDER_BGR` is set so
  that our RGB565 framebuffer data displays with correct colours.
- The backlight is driven via a BSS138 N-FET; GPIO 21 high = backlight on.
- RGB LED is common-anode (shared VCC3V3); drive low to illuminate.
- The `espressif/esp_lcd_ili9341` component is fetched from the IDF Component
  Registry on first build.
