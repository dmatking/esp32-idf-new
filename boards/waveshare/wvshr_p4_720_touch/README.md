# Waveshare ESP32-P4-WIFI6-Touch-LCD-4B

4.0" 720×720 IPS touchscreen module from Waveshare. MIPI-DSI display (ST7703, RGB888), GT911 capacitive touch, ESP32-C6 co-processor for WiFi 6 / BLE.

- **MCU:** ESP32-P4 @ 360 MHz
- **Co-processor:** ESP32-C6 (WiFi 6, BLE 5)
- **Memory:** 32MB PSRAM (HEX), 16MB flash
- **LCD:** ST7703 MIPI-DSI panel (720×720, RGB888), 2 lanes @ 480 Mbps
- **Touch:** GT911 capacitive (I²C)
- **Product page:** [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm)

## Pin Reference

| Signal         | GPIO |
| -------------- | ---- |
| Backlight      | 26 (active LOW) |
| LCD Reset      | 27 |
| MIPI PHY LDO   | channel 3, 2500 mV |

Touch (GT911) and WiFi (ESP32-C6) are handled via the MIPI-DSI interface and SDIO respectively — no additional GPIO config needed for basic display use.

## Notes

- **RGB888** — 3 bytes per pixel, framebuffer is 720×720×3 = ~1.5 MB. Allocate from PSRAM.
- **Backlight is active LOW** — `gpio_set_level(26, 0)` turns it on.
- **ESP32-C6 co-processor** ships with old firmware (v0.0.0) that does not support BT. The `terminal-p4` project OTAs the C6 to v2.12.3 on first boot.
- **DPI clock:** 38 MHz. Do not increase without testing — the panel is sensitive to clock speed.
