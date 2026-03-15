// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

// MAX98357A I2S amplifier speaker output
// I2S pins: LRCK=IO38, DIN=IO47, BCK=IO48

#include "speaker.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "SPEAKER";

#define I2S_PORT        I2S_NUM_1
#define I2S_PIN_BCK     48
#define I2S_PIN_WS      38   // LRCK
#define I2S_PIN_DOUT    47

#define SAMPLE_RATE     CONFIG_SPEAKER_SAMPLE_RATE
#define BITS_PER_SAMPLE CONFIG_SPEAKER_BITS_PER_SAMPLE

static i2s_chan_handle_t tx_handle = NULL;

esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(BITS_PER_SAMPLE, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCK,
            .ws   = I2S_PIN_WS,
            .dout = I2S_PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Speaker ready (%dHz, %d-bit)", SAMPLE_RATE, BITS_PER_SAMPLE);
    return ESP_OK;
}

esp_err_t speaker_write(const int16_t *samples, size_t num_samples, size_t *written)
{
    if (!tx_handle) return ESP_ERR_INVALID_STATE;
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, samples, num_samples * sizeof(int16_t),
                                      &bytes_written, portMAX_DELAY);
    if (written) *written = bytes_written / sizeof(int16_t);
    return ret;
}
