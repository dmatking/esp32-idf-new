// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOARD_NAME "Waveshare ESP32-S3 Touch LCD 1.85\""

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 360
#define LCD_V_RES 360

#define PIN_LCD_CLK  40
#define PIN_LCD_D0   46
#define PIN_LCD_D1   45
#define PIN_LCD_D2   42
#define PIN_LCD_D3   41
#define PIN_LCD_CS   21
#define PIN_LCD_BL   5

#define PIN_TOUCH_SDA 1
#define PIN_TOUCH_SCL 3
#define PIN_TOUCH_INT 4

#define TOUCH_I2C_PORT I2C_NUM_1
#define CST816_ADDR 0x15
#define CST816_DATA_REG 0x02

#define LCD_OPCODE_WRITE_CMD 0x02ULL
#define LCD_OPCODE_READ_CMD  0x0BULL
#define LCD_OPCODE_WRITE_COLOR 0x32ULL

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;
static SemaphoreHandle_t s_flush_sem = NULL;
static const char *TAG = "BOARD_WVSHR_1V85_T";
static bool s_touch_task_started = false;

static const st77916_lcd_init_cmd_t vendor_specific_init_touch[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
};

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
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_LCD_BL, 1);
}

static void init_touch_bus(void)
{
    const i2c_port_t port = TOUCH_I2C_PORT;
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(port, &cfg);
    i2c_driver_install(port, cfg.mode, 0, 0, 0);
    gpio_reset_pin(PIN_TOUCH_INT);
    gpio_set_direction(PIN_TOUCH_INT, GPIO_MODE_INPUT);
}

static esp_err_t cst816_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CST816_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CST816_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static bool cst816_get_point(uint16_t *x, uint16_t *y)
{
    uint8_t buf[6] = {0};
    if (cst816_read(CST816_DATA_REG, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    uint8_t points = buf[0] & 0x0F;
    if (points == 0) {
        return false;
    }
    uint16_t x_val = ((buf[1] & 0x0F) << 8) | buf[2];
    uint16_t y_val = ((buf[3] & 0x0F) << 8) | buf[4];
    if (x) {
        *x = x_val;
    }
    if (y) {
        *y = y_val;
    }
    return true;
}

static void touch_logger_task(void *arg)
{
    bool touching = false;
    while (1) {
        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = cst816_get_point(&x, &y);
        if (pressed) {
            touching = true;
            ESP_LOGI(TAG, "Touch (%u,%u)", (unsigned)x, (unsigned)y);
        } else if (touching) {
            touching = false;
            ESP_LOGI(TAG, "Touch released");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void start_touch_logger(void)
{
    if (s_touch_task_started) {
        return;
    }
    BaseType_t ok = xTaskCreate(touch_logger_task, "TouchLogger", 2048, NULL, 5, NULL);
    if (ok == pdPASS) {
        s_touch_task_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to start touch logger task");
    }
}

void board_init(void)
{
    ESP_LOGI(TAG, "%s init: ST77916 LCD + CST816 touch", BOARD_NAME);
    init_backlight();
    init_touch_bus();
    start_touch_logger();

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
    bool use_touch_init = false;
    {
        esp_lcd_panel_io_handle_t detect_io = NULL;
        esp_lcd_panel_io_spi_config_t detect_io_cfg = io_cfg;
        detect_io_cfg.pclk_hz = 3 * 1000 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &detect_io_cfg, &detect_io));

        uint8_t id[4] = {0};
        int lcd_cmd = 0x04;
        lcd_cmd &= 0xFF;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_READ_CMD << 24;
        esp_err_t read_err = esp_lcd_panel_io_rx_param(detect_io, lcd_cmd, id, sizeof(id));
        if (read_err == ESP_OK) {
            ESP_LOGI(TAG, "ST77916 ID: %02X %02X %02X %02X", id[0], id[1], id[2], id[3]);
            use_touch_init = (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F);
        } else {
            ESP_LOGW(TAG, "Failed to read ST77916 ID: %s", esp_err_to_name(read_err));
        }
        ESP_ERROR_CHECK(esp_lcd_panel_io_del(detect_io));
    }

    s_flush_sem = xSemaphoreCreateBinary();
    assert(s_flush_sem);
    io_cfg.on_color_trans_done = flush_done_cb;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    st77916_vendor_config_t vendor_cfg = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    if (use_touch_init) {
        vendor_cfg.init_cmds = vendor_specific_init_touch;
        vendor_cfg.init_cmds_size = sizeof(vendor_specific_init_touch) / sizeof(vendor_specific_init_touch[0]);
        ESP_LOGI(TAG, "Applying touch panel init sequence");
    } else {
        ESP_LOGI(TAG, "Using default ST77916 init sequence");
    }

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

    s_fb = heap_caps_aligned_calloc(4, LCD_H_RES * LCD_V_RES * sizeof(uint16_t), 1,
                MALLOC_CAP_DEFAULT);
    assert(s_fb);

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

void board_lcd_fill(uint16_t color)
{
    fill_screen(color);
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

void board_lcd_sanity_test(void)
{
    if (!s_panel) {
        ESP_LOGW(TAG, "Panel missing; skip sanity test");
        return;
    }
    ESP_LOGI(TAG, "Starting LCD sanity task...");
    xTaskCreate(lcd_sanity_task, "lcd_sanity", 4096, NULL, 4, NULL);
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
