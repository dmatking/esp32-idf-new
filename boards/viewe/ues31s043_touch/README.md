# VIEWE UES31S043 4.3" ESP32-S31 Smart Display

VIEWE PLCM-UES31S043H800V480C-U: ESP32-S31-WROOM-3 (N16R16V) with a 4.3" 800×480 RGB IPS panel
(ST7262) and GT911 capacitive touch. Vendor repo: <https://github.com/VIEWESMART/PLCM-UES31S043H800V480C-U>

| Detail      | Value                                                     |
| ----------- | --------------------------------------------------------- |
| MCU         | ESP32-S31 (dual-core RISC-V, 320 MHz)                     |
| Memory      | 16 MB QIO flash, 16 MB octal PSRAM (200 MHz)              |
| Display     | 4.3" IPS 800×480, RGB parallel (wired RGB666, driven 565) |
| Touch       | GT911 (I2C, SDA=GPIO0, SCL=GPIO1, addr 0x5D or 0x14)      |
| Backlight   | GPIO39, active high (on/off only)                         |
| Audio       | ES8389 codec (I2S GPIO48–52, shared I2C), PA_EN=GPIO47    |
| SD card     | SDMMC 4-bit, CLK=24, CMD=25, D0–D3=20–23                  |
| LED         | WS2812B on GPIO37                                         |
| Buttons     | 4 × ADC ladder on GPIO42                                  |
| RS485 / CAN | UART TX=35/RX=36, TWAI TX=53/RX=54                        |
| Console     | UART0 via CH340C on the USB-C port                        |

**Requires IDF 6.1** (`release/v6.1` branch); the `esp32s31` target is not in 6.0.x.

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
$env:PYTHONIOENCODING = "utf-8"
```

## Build

```powershell
idf-new my_project --board viewe/ues31s043_touch
cd my_project
idf.py build
idf.py flash
```

Flash and monitor through the USB-C port (CH340). The USB-A port is the USB 2.0 HS OTG port, not
a download port.

## Notes

- Pins and timing come from the vendor BSP `viewesmart/bsp_ues31s043h800v480c_u` **1.0.2**
  (22 MHz PCLK, H/V 4/8/8). The vendor README still lists the older 1.0.0 timing
  (18 MHz, 1/40/20 and 1/10/5). If the image is unstable, try 18 MHz first.
- The panel is wired RGB666 on GPIO2–19. This board uses a 16-bit RGB565 bus (R3–R7, G2–G7,
  B3–B7) so the framebuffer matches `board_interface.h`. R2 (GPIO2) and B2 (GPIO14) are held low,
  so full-scale red and blue peak at 0xF8 rather than 0xFF.
- Touch is optional: `board_init()` probes 0x5D then 0x14 and continues without touch if neither
  answers.
- SD power control: the vendor changelog says schematic V1.1 uses GPIO57 (`SD_CTRL`, low = on).
  The vendor README's GPIO60 is wrong for V1.1, and early boards have SD power always on.
- `board_lcd_flush()` uses `esp_cache_msync` to push CPU cache lines to PSRAM; the RGB DMA
  controller scans the framebuffer continuously.
