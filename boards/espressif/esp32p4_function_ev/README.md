# ESP32-P4 Function EV Board

Espressif's official ESP32-P4 evaluation board with a 4.3" 1024×600 IPS LCD.

| Detail      | Value                              |
| ----------- | ---------------------------------- |
| MCU         | ESP32-P4 @ 360 MHz (dual-core RISC-V) |
| Display     | EK79007 4.3" IPS 1024×600, MIPI-DSI  |
| Touch       | GT911 capacitive (I2C)             |
| WiFi / BT   | ESP32-C6 co-processor via SDIO     |
| PSRAM       | HEX mode @ 200 MHz                 |
| Flash       | 16 MB                              |
| Backlight   | GPIO 26, active HIGH               |
| Reset       | GPIO 27                            |

## Build

```bash
idf-new my_project --board espressif/esp32p4_function_ev
cd my_project
idf set-target esp32p4
idf build
idf flash
```
