// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "ESPS3_DEVKITC_NHD24"
#define BOARD_NAME "ESP32-S3-DevKitC + Newhaven 2.4\" IPS (ST7789 i80)"

#define LCD_H_RES 240
#define LCD_V_RES 320

// Data bus: DB8–DB15 mapped to safe GPIOs
#define LCD_DB8    4
#define LCD_DB9    5
#define LCD_DB10   6
#define LCD_DB11   7
#define LCD_DB12   15
#define LCD_DB13   16
#define LCD_DB14   17
#define LCD_DB15   18

// Control pins
#define LCD_PIN_CS   9
#define LCD_PIN_DC   10
#define LCD_PIN_WR   11
#define LCD_PIN_RST  12
// RDX is tied high in hardware

#define CHUNK_LINES  20
static uint16_t s_line_buf[LCD_H_RES * CHUNK_LINES];

static esp_lcd_i80_bus_handle_t   s_i80_bus  = NULL;
static esp_lcd_panel_io_handle_t  s_panel_io = NULL;
static esp_lcd_panel_handle_t     s_panel    = NULL;
static uint16_t *s_fb = NULL;
static SemaphoreHandle_t s_flush_sem = NULL;
static bool s_panel_ready = false;

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool flush_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return (woken == pdTRUE);
}

// Fill the whole screen with a solid RGB565 color
static void lcd_fill_color(uint16_t color)
{
    if (!s_panel_ready) return;

    for (int i = 0; i < LCD_H_RES * CHUNK_LINES; i++) {
        s_line_buf[i] = color;
    }

    for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
        int h = CHUNK_LINES;
        if (y + h > LCD_V_RES) {
            h = LCD_V_RES - y;
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(
            s_panel,
            0,
            y,
            LCD_H_RES,
            y + h,
            s_line_buf
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw_bitmap failed at y=%d: %s",
                     y, esp_err_to_name(err));
            return;
        }
    }
}

static void lcd_init(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Configuring I80 bus");

    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src        = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num    = LCD_PIN_DC,
        .wr_gpio_num    = LCD_PIN_WR,
        .data_gpio_nums = {
            LCD_DB8,
            LCD_DB9,
            LCD_DB10,
            LCD_DB11,
            LCD_DB12,
            LCD_DB13,
            LCD_DB14,
            LCD_DB15,
        },
        .bus_width         = 8,
        .max_transfer_bytes = LCD_H_RES * 100 * sizeof(uint16_t),
        // leave psram_trans_align/sram_trans_align/dma_burst_size as 0 (defaults)
    };

    err = esp_lcd_new_i80_bus(&bus_config, &s_i80_bus);
    ESP_LOGI(TAG, "esp_lcd_new_i80_bus() returned %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Configuring panel IO (i80)");

    s_flush_sem = xSemaphoreCreateBinary();
    assert(s_flush_sem);

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = 8 * 1000 * 1000,  // start conservative
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .on_color_trans_done = flush_done_cb,
        .dc_levels = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
        .flags = {
            .swap_color_bytes = 1,
            .pclk_active_neg = 0,
            .pclk_idle_low   = 0,
        },
    };

    err = esp_lcd_new_panel_io_i80(s_i80_bus, &io_config, &s_panel_io);
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Creating ST7789 panel driver");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel);
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Resetting and initializing panel");

    err = esp_lcd_panel_reset(s_panel);
    ESP_ERROR_CHECK(err);
    delay_ms(50);

    err = esp_lcd_panel_init(s_panel);
    ESP_LOGI(TAG, "esp_lcd_panel_init -> %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(err);

    // Try to normalize colors: make sure white is white, black is black
    ESP_LOGI(TAG, "Turning OFF color inversion");
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));

    // Portrait 240x320
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_panel_ready = true;

    s_fb = heap_caps_aligned_calloc(4, LCD_H_RES * LCD_V_RES * sizeof(uint16_t), 1,
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_fb);

    ESP_LOGI(TAG, "Panel init done");
}

void board_init(void)
{
    ESP_LOGI(TAG, "%s init", BOARD_NAME);
    lcd_init();
}

const char *board_get_name(void)
{
    return BOARD_NAME;
}

bool board_has_lcd(void)
{
    return s_panel_ready;
}

void board_lcd_sanity_test(void)
{
    if (!s_panel_ready) {
        ESP_LOGW(TAG, "Panel not ready; skipping sanity test");
        return;
    }

    ESP_LOGI(TAG, "Fill RED");
    lcd_fill_color(0xF800);
    delay_ms(500);

    ESP_LOGI(TAG, "Fill GREEN");
    lcd_fill_color(0x07E0);
    delay_ms(500);

    ESP_LOGI(TAG, "Fill BLUE");
    lcd_fill_color(0x001F);
    delay_ms(500);

    ESP_LOGI(TAG, "Fill WHITE");
    lcd_fill_color(0xFFFF);
    delay_ms(500);

    ESP_LOGI(TAG, "Fill BLACK");
    lcd_fill_color(0x0000);
}

// --- Display drawing API ---
// The i80 driver handles byte-swapping (swap_color_bytes=1), so the
// framebuffer stores standard RGB565 without manual byte swap.

int board_lcd_width(void) { return LCD_H_RES; }
int board_lcd_height(void) { return LCD_V_RES; }

void board_lcd_flush(void)
{
    if (!s_panel_ready || !s_fb) return;
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);
}

void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    if (!s_panel_ready || !s_fb) return;
    if (x1 == 0 && x2 == LCD_H_RES) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y1, LCD_H_RES, y2, &s_fb[y1 * LCD_H_RES]);
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    } else {
        for (int y = y1; y < y2; y++) {
            esp_lcd_panel_draw_bitmap(s_panel, x1, y, x2, y + 1, &s_fb[y * LCD_H_RES + x1]);
            xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        }
    }
}

void board_lcd_clear(void)
{
    if (s_fb) memset(s_fb, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
}

void board_lcd_set_pixel_raw(int x, int y, uint16_t color)
{
    if (s_fb) s_fb[y * LCD_H_RES + x] = color;
}

void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_fb) s_fb[y * LCD_H_RES + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

uint16_t board_lcd_get_pixel_raw(int x, int y)
{
    return s_fb ? s_fb[y * LCD_H_RES + x] : 0;
}

void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = ((color >> 11) & 0x1F) << 3;
    *g = ((color >> 5) & 0x3F) << 2;
    *b = (color & 0x1F) << 3;
}