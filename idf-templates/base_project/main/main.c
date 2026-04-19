// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board_interface.h"

static const char *TAG = "APP";

typedef struct {
    uint32_t period_ms;
} heartbeat_params_t;

static void heartbeat_task(void *pvParameters)
{
    heartbeat_params_t *params = (heartbeat_params_t *)pvParameters;
    const TickType_t period_ticks = pdMS_TO_TICKS(params->period_ms);

    const char *task_name = pcTaskGetName(NULL);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t run_count = 0;

    ESP_LOGI(TAG, "%s starting, period=%u ms", task_name, params->period_ms);

    while (1) {
        run_count++;
        ESP_LOGI(TAG, "%s run #%u (board: %s)",
                 task_name, (unsigned)run_count, board_get_name());
        vTaskDelayUntil(&last_wake, period_ticks);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main starting");
    board_init();

    static heartbeat_params_t hb = { .period_ms = 1000 };

    xTaskCreate(
        heartbeat_task,
        "Heartbeat",
        4096,
        &hb,
        5,
        NULL
    );

    if (board_has_lcd()) {
        ESP_LOGI(TAG, "Board has LCD, running sanity test...");
        while (1) {
            board_lcd_sanity_test();
        }
    } else {
        ESP_LOGI(TAG, "Board has no LCD configured.");
    }
}
