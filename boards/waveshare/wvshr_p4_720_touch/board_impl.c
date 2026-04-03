// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board implementation for Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
// 720x720 MIPI-DSI LCD (ST7703), GT911 capacitive touch, WiFi via ESP32-C6
//
// Display init ported from gh-stats-dashboard (verified working on hardware).

#include "board_interface.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "BOARD_WVSHR_P4_720";

#define BOARD_NAME "Waveshare ESP32-P4 720x720 Touch"

#define LCD_W  720
#define LCD_H  720
#define BPP    3   // RGB888

// MIPI-DSI config
#define DSI_LANE_NUM        2
#define DSI_LANE_MBPS       480
#define DSI_DPI_CLK_MHZ     38
#define DSI_PHY_LDO_CHAN    3
#define DSI_PHY_LDO_MV      2500
#define DSI_BK_LIGHT_GPIO   26   // active LOW: 0 = on
#define DSI_RST_GPIO        27

// Framebuffer size
#define FB_SIZE (LCD_W * LCD_H * BPP)

static esp_lcd_panel_handle_t  s_panel       = NULL;
static ppa_client_handle_t     s_ppa_srm     = NULL;
static SemaphoreHandle_t       s_flush_done  = NULL;
static bool                    s_flush_pend  = false;

static uint8_t *s_fb;        // hardware framebuffer (scanned by DSI, from panel)
static uint8_t *s_buf_a;     // double-buffer A  (render targets, PSRAM+DMA)
static uint8_t *s_buf_b;     // double-buffer B
static uint8_t *s_backbuf;   // current render buffer

// --- PPA async flush callback (ISR context) ---
static bool flush_done_cb(ppa_client_handle_t client,
                          ppa_event_data_t *event_data, void *user_data)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return (woken == pdTRUE);
}

// --- Internal flush helpers ---
static void flush_wait(void)
{
    if (s_flush_pend) {
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
        esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
        s_flush_pend = false;
    }
}

static void flush_async(void)
{
    flush_wait();

    // Swap backbuffers
    uint8_t *render = s_backbuf;
    s_backbuf = (render == s_buf_a) ? s_buf_b : s_buf_a;

    esp_cache_msync(render, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_srm_oper_config_t cfg = {
        .in  = { .buffer = render,  .pic_w = LCD_W, .pic_h = LCD_H,
                 .block_w = LCD_W,  .block_h = LCD_H,
                 .block_offset_x = 0, .block_offset_y = 0,
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB888 },
        .out = { .buffer = s_fb,    .buffer_size = FB_SIZE,
                 .pic_w = LCD_W,    .pic_h = LCD_H,
                 .block_offset_x = 0, .block_offset_y = 0,
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB888 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x        = 1.0f,
        .scale_y        = 1.0f,
        .mode           = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(s_ppa_srm, &cfg));
    s_flush_pend = true;
}

// --- RGB565 ↔ RGB888 helpers ---
// board_interface.h exposes uint16_t RGB565 as the "raw" pixel format.
// Internally we store BGR888 in the framebuffer (matching ST7703 byte order).

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

    // LDO for MIPI PHY
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    // MIPI-DSI bus
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t dsi_bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_bus_cfg, &dsi_bus));

    // DBI (command) interface
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io));

    // DPI (pixel) interface — timing verified on hardware
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_CLK_MHZ,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = LCD_W,
            .v_size            = LCD_H,
            .hsync_back_porch  = 50,
            .hsync_pulse_width = 20,
            .hsync_front_porch = 50,
            .vsync_back_porch  = 20,
            .vsync_pulse_width = 4,
            .vsync_front_porch = 20,
        },
        .flags = { .use_dma2d = true },
    };

    // use_mipi_interface = 1 is required — without it ST7703 inits in RGB mode
    st7703_vendor_config_t vendor_cfg = {
        .flags       = { .use_mipi_interface = 1 },
        .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DSI_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config  = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7703(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Backlight — active LOW on Waveshare board: 0 = on
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << DSI_BK_LIGHT_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(DSI_BK_LIGHT_GPIO, 0);

    // Get hardware framebuffer pointer from DPI panel
    void *fb0 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb0));
    s_fb = (uint8_t *)fb0;

    // Render buffers in PSRAM — must be DMA-capable for PPA
    s_buf_a = heap_caps_aligned_calloc(64, FB_SIZE, 1,
                  MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_buf_b = heap_caps_aligned_calloc(64, FB_SIZE, 1,
                  MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(s_buf_a && s_buf_b);
    s_backbuf = s_buf_a;
    memset(s_fb, 0, FB_SIZE);

    // PPA SRM client
    ppa_client_config_t ppa_cfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ppa_srm));

    s_flush_done = xSemaphoreCreateBinary();
    assert(s_flush_done);
    ppa_event_callbacks_t ppa_cbs = { .on_trans_done = flush_done_cb };
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_ppa_srm, &ppa_cbs));

    // Initial black frame
    flush_async();
    flush_wait();

    ESP_LOGI(TAG, "%s init done", BOARD_NAME);
}

const char *board_get_name(void) { return BOARD_NAME; }
bool        board_has_lcd(void)  { return s_panel != NULL; }

// --- LCD API ---

int board_lcd_width(void)  { return LCD_W; }
int board_lcd_height(void) { return LCD_H; }

void board_lcd_clear(void)
{
    if (s_backbuf) memset(s_backbuf, 0, FB_SIZE);
}

void board_lcd_flush(void)
{
    flush_async();
    flush_wait();
}

void board_lcd_fill(uint16_t color)
{
    if (!s_backbuf) return;
    uint8_t r, g, b;
    rgb565_to_rgb888(color, &r, &g, &b);
    uint8_t *p = s_backbuf;
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        p[0] = b; p[1] = g; p[2] = r;
        p += 3;
    }
    board_lcd_flush();
}

void board_lcd_set_pixel_raw(int x, int y, uint16_t color)
{
    if (!s_backbuf || x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    uint8_t r, g, b;
    rgb565_to_rgb888(color, &r, &g, &b);
    uint8_t *p = s_backbuf + (y * LCD_W + x) * BPP;
    p[0] = b; p[1] = g; p[2] = r;
}

void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_backbuf || x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    uint8_t *p = s_backbuf + (y * LCD_W + x) * BPP;
    p[0] = b; p[1] = g; p[2] = r;
}

uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return rgb888_to_rgb565(r, g, b);
}

uint16_t board_lcd_get_pixel_raw(int x, int y)
{
    if (!s_backbuf || x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return 0;
    uint8_t *p = s_backbuf + (y * LCD_W + x) * BPP;
    return rgb888_to_rgb565(p[2], p[1], p[0]);
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
        uint8_t *p = s_backbuf;
        for (int j = 0; j < LCD_W * LCD_H; j++) {
            p[0] = colors[i].b;
            p[1] = colors[i].g;
            p[2] = colors[i].r;
            p += 3;
        }
        board_lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}
