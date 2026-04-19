# M5Stack Tab5

5.0" 720×1280 IPS touchscreen tablet from M5Stack. MIPI-DSI display with integrated touch (ST7123 combined display+touch IC, post-Oct-2025 hardware revision), PWM backlight, and I²C IO expander for power gating.

- **MCU:** ESP32-P4 @ 360 MHz
- **Memory:** 32MB PSRAM, 16MB flash
- **LCD:** ST7123 MIPI-DSI panel (720×1280, RGB565), 2 lanes @ 965 Mbps
- **Touch:** Integrated in ST7123 (I²C, shared bus with IO expander)
- **Power gate:** PI4IOE5V6408 I²C IO expander (two units: 0x43, 0x44)
- **Product page:** [M5Stack Tab5](https://shop.m5stack.com/products/tab5)

## Pin Reference

| Signal         | GPIO |
| -------------- | ---- |
| Backlight      | 22 (PWM via LEDC, active HIGH) |
| I²C SDA        | 31 |
| I²C SCL        | 32 |
| MIPI PHY LDO   | channel 3, 2500 mV |

## Notes

- **Post-Oct-2025 hardware** — earlier Tab5 units used ILI9881C + GT911 touch. The ST7123 is a combined display+touch IC; this implementation targets the newer revision.
- **PWM backlight** — 12-bit LEDC on GPIO 22 (0–4095). Full brightness on init.
- **RGB565** — 2 bytes per pixel, framebuffer is 720×1280×2 = ~1.8 MB. Allocate from PSRAM.
- **DPI clock:** 70 MHz, DSI lanes at 965 Mbps.
- **Speaker pop** — SPK_EN is held low during IO expander init to suppress the speaker pop on boot.
- Init sequence derived from `M5Tab5-UserDemo` (MIT, M5Stack Technology CO LTD).
