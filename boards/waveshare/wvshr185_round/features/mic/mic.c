// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

// MSM261S4030H0R I2S digital microphone
// Pins: SCK=IO15, WS=IO2, SD=IO39, EN=IO12

#include "mic.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "MIC";

#define I2S_PORT        I2S_NUM_0
#define I2S_PIN_BCK     15
#define I2S_PIN_WS      2
#define I2S_PIN_DIN     39
#define MIC_EN_GPIO     12
#define SAMPLE_RATE     CONFIG_MIC_SAMPLE_RATE

static i2s_chan_handle_t rx_handle = NULL;

esp_err_t mic_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << MIC_EN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(MIC_EN_GPIO, 1);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Philips I2S, stereo 32-bit — MSM261 outputs 24-bit audio in 32-bit frame
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCK,
            .ws   = I2S_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_PIN_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Microphone ready (%dHz)", SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t mic_read(int16_t *samples, size_t num_samples, size_t *read_count)
{
    if (!rx_handle) return ESP_ERR_INVALID_STATE;

    static int32_t raw[2048];  // stereo 32-bit pairs
    size_t total = 0;

    while (total < num_samples) {
        size_t chunk = num_samples - total;
        if (chunk > 1024) chunk = 1024;

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_handle, raw, chunk * 2 * sizeof(int32_t),
                                         &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) return ret;

        size_t pairs = bytes_read / (2 * sizeof(int32_t));

        // MSM261 outputs on right channel (L/R pin tied high)
        // 24-bit audio in upper bits of 32-bit frame, shift to 16-bit
        for (size_t i = 0; i < pairs; i++) {
            samples[total + i] = (int16_t)(raw[i * 2 + 1] >> 14);
        }
        total += pairs;
    }

    if (read_count) *read_count = total;
    return ESP_OK;
}
