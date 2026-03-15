// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Mic + Speaker loopback demo for waveshare/wvshr185_round
//
// Records 3 seconds of audio via the MSM261S4030H0R microphone,
// then plays it back through the MAX98357A speaker amplifier.
// LCD color indicates state: red=recording, green=playing, black=pause.
//
// Required features: --feature mic --feature speaker
//
// To use: copy this file to your project's main/ alongside main.c,
// or replace main.c with this file (rename app_main as needed).

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board_interface.h"
#include "speaker.h"
#include "mic.h"

static const char *TAG = "DEMO";

#define SAMPLE_RATE     16000
#define RECORD_SECS     3

static int16_t rec_buf[SAMPLE_RATE * RECORD_SECS];

static void loopback_task(void *arg)
{
    ESP_ERROR_CHECK(mic_init());
    ESP_ERROR_CHECK(speaker_init());

    const size_t n = sizeof(rec_buf) / sizeof(rec_buf[0]);
    // Discard buffer: absorbs residual speaker sound at start of each cycle
    static int16_t discard[SAMPLE_RATE / 2];

    while (1) {
        board_lcd_fill(0xF800);  // red = recording

        // Discard first 0.5s (tail of previous playback)
        size_t dummy;
        mic_read(discard, SAMPLE_RATE / 2, &dummy);

        ESP_LOGI(TAG, "Recording %ds...", RECORD_SECS);
        size_t got;
        mic_read(rec_buf, n, &got);

        board_lcd_fill(0x07E0);  // green = playing back
        ESP_LOGI(TAG, "Playing back %u samples", (unsigned)got);
        size_t written;
        speaker_write(rec_buf, got, &written);

        // Pause so speaker sound dies before mic opens again
        board_lcd_fill(0x0000);  // black = waiting
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_main(void)
{
    board_init();
    xTaskCreate(loopback_task, "loopback", 4096, NULL, 5, NULL);
}
