// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOARD_NAME "Waveshare ESP32-S3 LCD 1.85\""

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 360
#define LCD_V_RES 360

#define PIN_LCD_TE   18
#define PIN_LCD_CLK  40
#define PIN_LCD_D0   46
#define PIN_LCD_D1   45
#define PIN_LCD_D2   42
#define PIN_LCD_D3   41
#define PIN_LCD_CS   21
#define PIN_LCD_BL   5

static esp_lcd_panel_handle_t s_panel = NULL;
static const char *TAG = "BOARD_WVSHR_1V85";

static inline uint16_t swap_bytes_to_panel_color(uint16_t color)
{
    return (color << 8) | (color >> 8);
}

static void fill_screen(uint16_t color)
{
    if (!s_panel) {
        return;
    }

    static uint16_t line[LCD_H_RES];
    uint16_t panel_color = swap_bytes_to_panel_color(color);
    for (int x = 0; x < LCD_H_RES; ++x) {
        line[x] = panel_color;
    }

    for (int y = 0; y < LCD_V_RES; ++y) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, line);
    }
}

static void init_backlight(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_LCD_BL, 1);
}

void board_init(void)
{
    ESP_LOGI(TAG, "%s init: ST77916 360x360 round LCD", BOARD_NAME);
    init_backlight();

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_CLK,
        .mosi_io_num = PIN_LCD_D0,
        .miso_io_num = PIN_LCD_D1,
        .quadwp_io_num = PIN_LCD_D2,
        .quadhd_io_num = PIN_LCD_D3,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t) + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = -1,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .flags = {
            .quad_mode = 1,
            .sio_mode = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    st77916_vendor_config_t vendor_cfg = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "%s init done", BOARD_NAME);
}

const char *board_get_name(void)
{
    return BOARD_NAME;
}

bool board_has_lcd(void)
{
    return s_panel != NULL;
}

static void lcd_sanity_task(void *arg)
{
    static const uint16_t colors[] = {
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFFF, // white
        0x0000, // black
    };
    const int n = sizeof(colors) / sizeof(colors[0]);
    int i = 0;
    while (1) {
        fill_screen(colors[i]);
        i = (i + 1) % n;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void board_lcd_fill(uint16_t color)
{
    fill_screen(color);
}

void board_lcd_sanity_test(void)
{
    if (!s_panel) {
        ESP_LOGW(TAG, "Panel missing; skip sanity test");
        return;
    }
    ESP_LOGI(TAG, "Starting LCD sanity task...");
    xTaskCreate(lcd_sanity_task, "lcd_sanity", 4096, NULL, 4, NULL);
}
