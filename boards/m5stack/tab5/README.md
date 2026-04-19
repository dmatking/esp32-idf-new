# M5Stack Tab5

5.0" 720×1280 IPS touchscreen tablet from M5Stack. MIPI-DSI display (ILI9881C, RGB565), GT911 capacitive touch, LCD and touch power gated via PI4IOE5V6408 I2C IO expander.

- **MCU:** ESP32-P4 @ 360 MHz
- **Memory:** 32MB PSRAM, 16MB flash
- **LCD:** ILI9881C MIPI-DSI panel (720×1280, RGB565), 2 lanes @ 1000 Mbps
- **Touch:** GT911 capacitive (I²C, shared bus with IO expander)
- **Power gate:** PI4IOE5V6408 I2C IO expander (addr 0x43) controls LCD and touch power
- **Product page:** [M5Stack Tab5](https://shop.m5stack.com/products/tab5)

## Pin Reference

| Signal         | GPIO |
| -------------- | ---- |
| Backlight      | 22 (active HIGH) |
| I²C SDA        | 31 |
| I²C SCL        | 32 |
| MIPI PHY LDO   | channel 3, 2500 mV |

IO expander pins (via I²C):

| Expander pin | Function  |
| ------------ | --------- |
| 4            | LCD power enable |
| 5            | Touch power enable |

## Notes

- **IO expander must be initialized first** — LCD and touch power are gated behind the PI4IOE5V6408. Without it, the ILI9881C init sequence will time out.
- **RGB565** — 2 bytes per pixel, framebuffer is 720×1280×2 = ~1.8 MB. Allocate from PSRAM.
- **DPI clock:** 60 MHz, DSI lanes at 1000 Mbps.
- Init sequence ported from the Espressif `esp-bsp` (verified on hardware).
