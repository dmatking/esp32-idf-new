// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOARD_NAME "Waveshare ESP32-S3 Touch LCD 2.0\""

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 320

#define PIN_LCD_SCLK 39
#define PIN_LCD_MOSI 38
#define PIN_LCD_MISO 40
#define PIN_LCD_DC   42
#define PIN_LCD_CS   45
#define PIN_LCD_RST  -1
#define PIN_LCD_BL   1

#define PIN_TOUCH_SDA 48
#define PIN_TOUCH_SCL 47
#define TOUCH_I2C_PORT I2C_NUM_0

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static uint16_t *s_fb = NULL;
static SemaphoreHandle_t s_flush_sem = NULL;
static const char *TAG = "BOARD_WVSHR_2V0_T";
static bool s_touch_task_started = false;

static inline uint16_t swap_bytes_to_panel_color(uint16_t color)
{
    return (color << 8) | (color >> 8);
}

static inline uint16_t pack_rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static bool flush_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return (woken == pdTRUE);
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

static void start_touch_logger(void);

static void init_touch(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, cfg.mode, 0, 0, 0));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = 0;  // legacy driver ignores this; must be 0 to avoid ESP_ERR_INVALID_ARG
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_I2C_PORT, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_V_RES,
        .y_max = LCD_H_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io, &touch_cfg, &s_touch));
    start_touch_logger();
}

static void touch_logger_task(void *arg)
{
    bool touching = false;
    while (1) {
        if (s_touch) {
            esp_lcd_touch_point_data_t points_data[1] = {0};
            uint8_t point_count = 0;
            esp_lcd_touch_read_data(s_touch);
            esp_err_t err = esp_lcd_touch_get_data(s_touch, points_data, &point_count, 1);
            bool pressed = (err == ESP_OK) && (point_count > 0);
            if (pressed) {
                touching = true;
                ESP_LOGI(TAG, "Touch (%u,%u)", (unsigned)points_data[0].x, (unsigned)points_data[0].y);
            } else if (touching) {
                touching = false;
                ESP_LOGI(TAG, "Touch released");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void start_touch_logger(void)
{
    if (s_touch_task_started) {
        return;
    }
    BaseType_t ok = xTaskCreate(touch_logger_task, "TouchLogger2", 2048, NULL, 5, NULL);
    if (ok == pdPASS) {
        s_touch_task_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to start touch logger task");
    }
}

void board_init(void)
{
    ESP_LOGI(TAG, "%s init: ST7789 240x320 LCD + CST816 touch", BOARD_NAME);
    init_backlight();

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t) + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    s_flush_sem = xSemaphoreCreateBinary();
    assert(s_flush_sem);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .pclk_hz = 20 * 1000 * 1000,  // conservative to reduce noise/garbage
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = flush_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));

    s_fb = heap_caps_aligned_calloc(4, LCD_H_RES * LCD_V_RES * sizeof(uint16_t), 1,
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_fb);

    init_touch();

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

void board_lcd_sanity_test(void)
{
    if (!s_panel) {
        ESP_LOGW(TAG, "Panel missing; skip sanity test");
        return;
    }

    uint16_t red = 0xF800;
    uint16_t green = 0x07E0;
    uint16_t blue = 0x001F;
    uint16_t black = 0x0000;

    fill_screen(red);
    vTaskDelay(pdMS_TO_TICKS(500));
    fill_screen(green);
    vTaskDelay(pdMS_TO_TICKS(500));
    fill_screen(blue);
    vTaskDelay(pdMS_TO_TICKS(500));
    fill_screen(black);
}

// --- Display drawing API ---

int board_lcd_width(void) { return LCD_H_RES; }
int board_lcd_height(void) { return LCD_V_RES; }

void board_lcd_flush(void)
{
    if (!s_panel || !s_fb) return;
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);
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
    if (s_fb) s_fb[y * LCD_H_RES + x] = pack_rgb565_swapped(r, g, b);
}

uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return pack_rgb565_swapped(r, g, b);
}

uint16_t board_lcd_get_pixel_raw(int x, int y)
{
    return s_fb ? s_fb[y * LCD_H_RES + x] : 0;
}

void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    color = (color >> 8) | (color << 8);
    *r = ((color >> 11) & 0x1F) << 3;
    *g = ((color >> 5) & 0x3F) << 2;
    *b = (color & 0x1F) << 3;
}
