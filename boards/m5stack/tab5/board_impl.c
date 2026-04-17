// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board implementation for M5Stack Tab5 (ESP32-P4)
// 720x1280 MIPI-DSI LCD (ILI9881C, RGB565), GT911 capacitive touch
// LCD/touch power gated via PI4IOE5V6408 I2C IO expander
//
// Init sequence ported from Espressif esp-bsp (verified on hardware).

#include "board_interface.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "disp_init_data.h"

static const char *TAG = "BOARD_TAB5";

#define BOARD_NAME  "M5Stack Tab5"

#define LCD_W  720
#define LCD_H  1280
#define BPP    2    // RGB565

// MIPI-DSI config — from Espressif BSP / display.h
#define DSI_LANE_NUM        2
#define DSI_LANE_MBPS       1000
#define DSI_DPI_CLK_MHZ     60
#define DSI_PHY_LDO_CHAN    3
#define DSI_PHY_LDO_MV      2500

// GPIO
#define BK_LIGHT_GPIO       22   // active HIGH

// I2C (shared bus for IO expander and touch)
#define I2C_SCL_GPIO        32
#define I2C_SDA_GPIO        31
#define I2C_SPEED_HZ        400000

// PI4IOE5V6408 IO expander
#define IO_EXP_ADDR         0x43  // ADDRESS_LOW
#define IO_EXP_REG_OUTPUT   0x01
#define IO_EXP_REG_CONFIG   0x03
#define IO_EXP_PIN_LCD_EN   4
#define IO_EXP_PIN_TOUCH_EN 5

#define FB_SIZE (LCD_W * LCD_H * BPP)

static esp_lcd_panel_handle_t  s_panel   = NULL;
static uint8_t                *s_fb      = NULL;  // hardware framebuffer (DPI)
static uint8_t                *s_backbuf = NULL;  // render buffer (PSRAM)

// --- IO expander: enable LCD and touch power via PI4IOE5V6408 ---
// Must run before LDO/DSI init — without power the ILI9881C init will time out.
static void io_expander_enable_lcd_touch(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IO_EXP_ADDR,
        .scl_speed_hz    = I2C_SPEED_HZ,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

    // Set output register first (all pins high) before switching to output mode
    uint8_t out_cmd[] = {IO_EXP_REG_OUTPUT, 0xFF};
    ESP_ERROR_CHECK(i2c_master_transmit(dev, out_cmd, 2, 100));

    // Set pins 4 (LCD_EN) and 5 (TOUCH_EN) as outputs; leave others as inputs
    // Config reg: 0=output, 1=input. Clear bits 4 and 5 → 0xCF
    uint8_t dir_cmd[] = {IO_EXP_REG_CONFIG, 0xCF};
    ESP_ERROR_CHECK(i2c_master_transmit(dev, dir_cmd, 2, 100));

    ESP_ERROR_CHECK(i2c_master_bus_rm_device(dev));
}

// --- RGB565 helpers ---

static inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
}

static inline void rgb565_to_rgb888(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >>  5) & 0x3F) << 2;
    *b = ((c >>  0) & 0x1F) << 3;
}

// --- board_interface.h implementation ---

void board_init(void)
{
    ESP_LOGI(TAG, "%s init", BOARD_NAME);

    // Step 1: I2C bus (shared by IO expander and touch)
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port              = I2C_NUM_0,
        .sda_io_num            = I2C_SDA_GPIO,
        .scl_io_num            = I2C_SCL_GPIO,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    // Step 2: IO expander — power LCD and touch before DSI init
    io_expander_enable_lcd_touch(i2c_bus);
    vTaskDelay(pdMS_TO_TICKS(10));  // allow power rails to settle

    // Step 3: LDO for MIPI PHY
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    // Step 4: MIPI-DSI bus
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t dsi_bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_bus_cfg, &dsi_bus));

    // Step 5: DBI (command) interface
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io));

    // Step 6: DPI (pixel) interface — timing from Espressif BSP (verified)
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_CLK_MHZ,
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = LCD_W,
            .v_size            = LCD_H,
            .hsync_back_porch  = 140,
            .hsync_pulse_width = 40,
            .hsync_front_porch = 40,
            .vsync_back_porch  = 20,
            .vsync_pulse_width = 4,
            .vsync_front_porch = 20,
        },
        .flags = { .use_dma2d = true },
    };

    ili9881c_vendor_config_t vendor_cfg = {
        .init_cmds      = disp_init_data,
        .init_cmds_size = sizeof(disp_init_data) / sizeof(disp_init_data[0]),
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = DSI_LANE_NUM,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num       = GPIO_NUM_NC,
        .flags.reset_active_high = 0,
        .rgb_ele_order        = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel       = 16,
        .vendor_config        = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Step 7: Backlight — GPIO22, active HIGH
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << BK_LIGHT_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(BK_LIGHT_GPIO, 1);

    // Step 8: Hardware framebuffer from DPI panel
    void *fb0 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb0));
    s_fb = (uint8_t *)fb0;

    // Step 9: Render buffer in PSRAM (DMA-capable for cache sync)
    s_backbuf = heap_caps_aligned_calloc(64, FB_SIZE, 1,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(s_backbuf);
    memset(s_fb, 0, FB_SIZE);
    esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ESP_LOGI(TAG, "%s init done", BOARD_NAME);
}

const char *board_get_name(void) { return BOARD_NAME; }
bool        board_has_lcd(void)  { return s_panel != NULL; }

int board_lcd_width(void)  { return LCD_W; }
int board_lcd_height(void) { return LCD_H; }

void board_lcd_clear(void)
{
    if (s_backbuf) memset(s_backbuf, 0, FB_SIZE);
}

void board_lcd_flush(void)
{
    if (!s_backbuf || !s_fb) return;
    memcpy(s_fb, s_backbuf, FB_SIZE);
    esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

void board_lcd_fill(uint16_t color)
{
    if (!s_backbuf) return;
    uint16_t *p = (uint16_t *)s_backbuf;
    for (int i = 0; i < LCD_W * LCD_H; i++) p[i] = color;
    board_lcd_flush();
}

void board_lcd_set_pixel_raw(int x, int y, uint16_t color)
{
    if (!s_backbuf || x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    ((uint16_t *)s_backbuf)[y * LCD_W + x] = color;
}

void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    board_lcd_set_pixel_raw(x, y, rgb888_to_rgb565(r, g, b));
}

uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return rgb888_to_rgb565(r, g, b);
}

uint16_t board_lcd_get_pixel_raw(int x, int y)
{
    if (!s_backbuf || x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return 0;
    return ((uint16_t *)s_backbuf)[y * LCD_W + x];
}

void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    rgb565_to_rgb888(color, r, g, b);
}

void board_lcd_sanity_test(void)
{
    if (!s_panel) return;
    ESP_LOGI(TAG, "Sanity test: cycling colors");
    static const struct { uint8_t r, g, b; } colors[] = {
        {255,   0,   0},
        {  0, 255,   0},
        {  0,   0, 255},
        {255, 255, 255},
        {  0,   0,   0},
    };
    for (int i = 0; i < 5; i++) {
        uint16_t px = rgb888_to_rgb565(colors[i].r, colors[i].g, colors[i].b);
        uint16_t *p = (uint16_t *)s_backbuf;
        for (int j = 0; j < LCD_W * LCD_H; j++) p[j] = px;
        board_lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}
