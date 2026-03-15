// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

// TF (micro-SD) card: TCA9554PWR IO expander + SDMMC native 1-bit
// IO expander holds SD D3/CS HIGH, enabling native SDMMC mode.

#include "tf_card.h"

#include "driver/i2c.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

static const char *TAG = "TF_CARD";

// I2C bus (shared with IMU, RTC, IO expander)
#define I2C_PORT        0
#define I2C_PIN_SCL     10
#define I2C_PIN_SDA     11

// TCA9554PWR IO expander
#define TCA9554_ADDR        0x20
#define TCA9554_CONFIG_REG  0x03    // 0=output, 1=input per bit

// SDMMC native 1-bit pins
#define SD_PIN_CLK  14
#define SD_PIN_CMD  17
#define SD_PIN_D0   16

static sdmmc_card_t *s_card = NULL;

static esp_err_t init_i2c_bus(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_PIN_SDA,
        .scl_io_num       = I2C_PIN_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t ret = i2c_param_config(I2C_PORT, &cfg);
    if (ret != ESP_OK) return ret;
    ret = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    // Already installed by another feature — that's fine
    if (ret == ESP_ERR_INVALID_STATE) ret = ESP_OK;
    return ret;
}

static esp_err_t tca9554_init(void)
{
    // Set all 8 pins as outputs (CONFIG_REG = 0x00)
    // Power-on default output register is 0xFF, so D3/CS stays HIGH
    uint8_t buf[2] = {TCA9554_CONFIG_REG, 0x00};
    return i2c_master_write_to_device(I2C_PORT, TCA9554_ADDR, buf, sizeof(buf),
                                      pdMS_TO_TICKS(50));
}

esp_err_t tf_card_init(void)
{
    esp_err_t ret;

    ret = init_i2c_bus();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tca9554_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "IO expander ready (D3/CS=HIGH, native SDMMC mode)");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = CONFIG_TF_CARD_MAX_FILES,
        .allocation_unit_size   = 16 * 1024,
    };

    const char *mount_point = CONFIG_TF_CARD_MOUNT_POINT;
    ESP_LOGI(TAG, "Mounting SD card at %s ...", mount_point);
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    return ESP_OK;
}

const char *tf_card_get_mount_point(void)
{
    return CONFIG_TF_CARD_MOUNT_POINT;
}
