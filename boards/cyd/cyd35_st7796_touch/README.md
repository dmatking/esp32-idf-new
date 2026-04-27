# CYD 3.5" ST7796 Touch — `cyd35_st7796_touch`

LCDWIKI E32R35T / E32N35T — 3.5" 320×480 IPS display with ST7796 driver IC,
XPT2046 resistive touch, RGB LED, microSD slot, and audio amplifier on an
ESP32-WROOM-32E module.

## Pin Assignment

### LCD (ST7796) + Touch (XPT2046) — shared SPI2 host

Both the display and touch controller share SPI2_HOST (MOSI=13, SCLK=14, MISO=12).
They are multiplexed via separate CS pins.

| Signal    | GPIO |
|-----------|------|
| SCLK      | 14   |
| MOSI      | 13   |
| MISO      | 12   |
| LCD DC    | 2    |
| LCD CS    | 15   |
| LCD RST   | —    |
| BL        | 27 (high = on) |
| Touch CS  | 33   |
| Touch IRQ | 36 (input-only, low = touched) |

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

`board_impl.c` logs raw XPT2046 ADC values (0–4095) on each tap. The default
calibration constants (`CAL_SX0/SX1/SY0/SY1 = 200/3800`) are approximate — tap
the four corners and adjust from the `touch raw_x= raw_y=` serial log entries.

## Notes

- ST7796 panels have BGR physical connections; `LCD_RGB_ELEMENT_ORDER_BGR` is set
  so that our RGB565 framebuffer data displays with correct colours.
- The backlight is driven via a BSS138 N-FET; GPIO 27 high = backlight on.
- RGB LED is common-anode (shared VCC3V3); drive low to illuminate.
- LCD and touch share SPI2_HOST; `init_touch()` calls `spi_bus_add_device` only —
  the bus is already initialised by `board_init()`.
- The full 480×320 framebuffer is ~300 KB. On the classic ESP32 (520 KB internal
  SRAM) this leaves roughly 200 KB for the rest of the application. Monitor heap
  usage if adding WiFi or large buffers.
- The `espressif/esp_lcd_st7796` component is fetched from the IDF Component
  Registry on first build.
