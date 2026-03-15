# Waveshare ESP32-S3 Round Touch LCD 1.85"

[Goto Home](/README.md)

Touch-enabled variant of the Waveshare ESP32-S3 round LCD kit. Ships with a CST816S capacitive touch controller bonded to the 1.85" ST77916 (360x360) panel. All other peripherals are identical to the non-touch version.

Waveshare ESP32-S3 1.85" round with and without touch [Waveshare 1.85"](https://www.waveshare.com/product/arduino/boards-kits/esp32-s3/esp32-s3-touch-lcd-1.85.htm)

- **MCU:** ESP32-S3-WROOM-1
- **LCD:** 1.85" 360x360 ST77916 (QSPI + TE)
- **Touch:** CST816S capacitive touch (I2C)
- **Backlight:** LEDC PWM on GPIO5 (active high)
- **Reset:** LCD and touch reset lines driven through TCA9554 expander (EXIO1=touch, EXIO2=LCD)

## Pin Reference

### ST77916 QSPI Panel (360x360)

| Signal | GPIO |
| ------ | ---- |
| CLK    | IO40 |
| D0     | IO46 |
| D1     | IO45 |
| D2     | IO42 |
| D3     | IO41 |
| CS     | IO21 |
| TE     | IO18 |
| RST    | TCA9554 EXIO2 (not a native GPIO) |
| BLK    | IO5 (LEDC low-speed channel 0) |

### CST816S Touch (I2C_NUM_1, 400 kHz, address 0x15)

| Signal | GPIO |
| ------ | ---- |
| SDA    | IO1  |
| SCL    | IO3  |
| INT    | IO4  |
| RST    | TCA9554 EXIO1 (not a native GPIO) |

### I2C Bus (I2C_NUM_0, 400 kHz, shared by IMU, RTC, IO expander)

| Signal | GPIO |
| ------ | ---- |
| SCL    | IO10 |
| SDA    | IO11 |

### IMU -- QMI8658 (I2C address 0x6B)

Accel + gyro, 6-axis. Uses shared I2C bus above.

### RTC -- PCF85063 (I2C address 0x51)

Uses shared I2C bus above. Interrupt on IO9 (active low, not currently used in driver).

### IO Expander -- TCA9554PWR (I2C address 0x20)

Uses shared I2C bus above. Holds SD D3/CS HIGH for native SDMMC mode. Also drives LCD RST (EXIO2) and touch RST (EXIO1).

### TF Card (SDMMC native 1-bit)

| Signal | GPIO |
| ------ | ---- |
| CLK    | IO14 |
| CMD    | IO17 |
| D0     | IO16 |

SD D3/CS held HIGH by TCA9554 IO expander.

### Speaker -- MAX98357A (I2S_NUM_1)

| Signal | GPIO |
| ------ | ---- |
| BCK    | IO48 |
| LRCK   | IO38 |
| DIN    | IO47 |

### Microphone -- MSM261S4030H0R (I2S_NUM_0)

| Signal | GPIO |
| ------ | ---- |
| BCK    | IO15 |
| WS     | IO2  |
| SD     | IO39 |
| EN     | IO12 (active high) |

### UART Header (UART_NUM_1, 115200 baud)

| Signal | GPIO |
| ------ | ---- |
| TX     | IO43 |
| RX     | IO44 |

## Notes

- The board implementation reads ST77916 register `0x04` at a low (3 MHz) SPI clock. If the returned ID matches `00 02 7F 7F`, the code uploads a vendor-specific init table. Panels reporting other IDs fall back to the default Espressif init sequence.
- Touch I2C runs on I2C_NUM_1 (separate from the sensor/expander bus on I2C_NUM_0). A polling task logs coordinates for quick validation; hook your own driver if you need event routing.
- Reset lines for both LCD and touch are driven through the TCA9554 expander -- there is no dedicated ESP32 GPIO for reset.

## Verified Working

- [x] LCD (ST77916 QSPI, 360x360)
- [x] Touch (CST816S)
- [x] IMU (QMI8658, accel + gyro)
- [x] RTC (PCF85063)
- [x] Speaker (MAX98357A I2S DAC)
- [x] Microphone (MSM261S4030H0R I2S)
- [x] UART header (TX=IO43, RX=IO44)
- [x] TF card (TCA9554PWR IO expander + SDMMC native 1-bit)
- [ ] I2C header (SCL=IO10, SDA=IO11, shared bus with IMU/RTC/IO expander)
