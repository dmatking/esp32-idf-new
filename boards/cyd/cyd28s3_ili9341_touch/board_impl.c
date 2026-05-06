// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "led_strip.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOARD_NAME "CYD-S3 2.8\" ILI9341 (ES3C28P)"

// --- LCD (ILI9341V) on SPI2_HOST (FSPI) ---
#define LCD_HOST     SPI2_HOST
#define LCD_H_RES    320   // landscape
#define LCD_V_RES    240
#define PIN_LCD_SCLK 12
#define PIN_LCD_MOSI 11
#define PIN_LCD_MISO 13
#define PIN_LCD_DC   46
#define PIN_LCD_CS   10
#define PIN_LCD_RST  -1
#define PIN_LCD_BL   45   // high = backlight on (via BSS138 N-FET)

// --- Touch (FT6336G) on I2C0 ---
#define TOUCH_I2C_PORT I2C_NUM_0
#define PIN_TOUCH_SCL  15
#define PIN_TOUCH_SDA  16
#define PIN_TOUCH_INT  17   // active low
#define PIN_TOUCH_RST  18   // active low

// --- WS2812B RGB LED ---
#define PIN_LED_DATA   42
#define LED_NUM_PIXELS 1

static esp_lcd_panel_handle_t  s_panel   = NULL;
static esp_lcd_touch_handle_t  s_touch   = NULL;
static led_strip_handle_t      s_led     = NULL;
static uint16_t               *s_fb      = NULL;
static SemaphoreHandle_t       s_flush_sem = NULL;
static const char             *TAG       = "BOARD_CYD28S3";

// ---------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------

static inline uint16_t swap_bytes(uint16_t c)
{
    return (c << 8) | (c >> 8);
}

static inline uint16_t pack_rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
    return swap_bytes(c);
}

// ---------------------------------------------------------------------------
// SPI flush callback
// ---------------------------------------------------------------------------

static bool flush_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return (woken == pdTRUE);
}

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// WS2812B LED
// ---------------------------------------------------------------------------

static void init_led(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_LED_DATA,
        .max_leds = LED_NUM_PIXELS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812B init failed");
        s_led = NULL;
        return;
    }
    // Brief dim-white flash at init to confirm the LED works, then clear.
    led_strip_set_pixel(s_led, 0, 16, 16, 16);
    led_strip_refresh(s_led);
    vTaskDelay(pdMS_TO_TICKS(200));
    led_strip_clear(s_led);
}

// ---------------------------------------------------------------------------
// FT6336G capacitive touch (I2C, address 0x38)
// ---------------------------------------------------------------------------

static void init_touch(void)
{
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        // Board has 4.7K hardware pull-ups; software pull-ups add extra safety margin.
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    // Legacy I2C driver ignores scl_speed_hz; must be 0 to avoid ESP_ERR_INVALID_ARG.
    tp_io_cfg.scl_speed_hz = 0;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
        (esp_lcd_i2c_bus_handle_t)TOUCH_I2C_PORT, &tp_io_cfg, &tp_io));

    // FT6336G raw axes: IC-x = vertical (portrait, high=top), IC-y = horizontal (portrait).
    // Portrait panel is displayed landscape (LCD swap_xy=true). Touch transform:
    //   mirror_x flips IC-x so 0=top, then swap_xy maps IC-y→display-x, IC-x→display-y.
    // x_max must be IC portrait x maximum (239), not LCD_H_RES.
    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_V_RES - 1,   // IC portrait x max = 239
        .y_max = LCD_H_RES - 1,   // IC portrait y max = 319 (unused, mirror_y=0)
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &touch_cfg, &s_touch));
}

// ---------------------------------------------------------------------------
// Minimal 5×7 bitmap font (column-major, bit 0 = top row).
// ---------------------------------------------------------------------------
#define FONT_W  5
#define FONT_H  7
#define FONT_GAP 1

typedef struct { char ch; uint8_t cols[FONT_W]; } glyph_t;

static const glyph_t s_glyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'0', {0x3E,0x51,0x49,0x45,0x3E}},
    {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}},
    {'3', {0x21,0x41,0x45,0x4B,0x31}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}},
    {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1E}},
    {':', {0x00,0x36,0x36,0x00,0x00}},
    {'X', {0x63,0x14,0x08,0x14,0x63}},
    {'Y', {0x07,0x08,0x70,0x08,0x07}},
};

static const uint8_t *find_glyph(char c)
{
    for (int i = 0; i < (int)(sizeof(s_glyphs)/sizeof(s_glyphs[0])); i++)
        if (s_glyphs[i].ch == c) return s_glyphs[i].cols;
    return s_glyphs[0].cols;
}

static void draw_char_scaled(int x, int y, char c, uint16_t color, int scale)
{
    const uint8_t *cols = find_glyph(c);
    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            if (!(cols[col] & (1 << row))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    board_lcd_set_pixel_raw(x + col*scale + dx,
                                           y + row*scale + dy, color);
        }
    }
}

static void draw_string_scaled(int x, int y, const char *s, uint16_t color, int scale)
{
    while (*s) {
        draw_char_scaled(x, y, *s++, color, scale);
        x += (FONT_W + FONT_GAP) * scale;
    }
}

// ---------------------------------------------------------------------------
// board_init
// ---------------------------------------------------------------------------

void board_init(void)
{
    ESP_LOGI(TAG, "%s init: ILI9341 240x320 IPS + FT6336G cap-touch", BOARD_NAME);
    init_backlight();
    init_led();

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
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = flush_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        // ILI9341 panels have BGR physical connections.
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // Landscape, USB connector on right — same orientation as cyd28.
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    // IPS panel requires color inversion (ILI9341V on IPS vs TN).
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_fb = heap_caps_aligned_calloc(4, LCD_H_RES * LCD_V_RES * sizeof(uint16_t), 1,
               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_fb);

    init_touch();

    ESP_LOGI(TAG, "%s init done", BOARD_NAME);
}

// ---------------------------------------------------------------------------
// Board identity
// ---------------------------------------------------------------------------

const char *board_get_name(void)
{
    return BOARD_NAME;
}

bool board_has_lcd(void)
{
    return s_panel != NULL;
}

// ---------------------------------------------------------------------------
// Sanity test — arrows + live capacitive touch coordinate display
// ---------------------------------------------------------------------------

static void draw_arrow_right(int ox, int oy, uint16_t color)
{
    const int tip_x       = ox + 75;
    const int head_base_x = ox;
    const int head_half_h = 45;
    const int stem_half_h = 10;
    const int stem_left_x = ox - 70;

    for (int x = head_base_x; x <= tip_x; x++) {
        int half = ((tip_x - x) * head_half_h) / (tip_x - head_base_x);
        for (int y = oy - half; y <= oy + half; y++)
            board_lcd_set_pixel_raw(x, y, color);
    }
    for (int x = stem_left_x; x < head_base_x; x++)
        for (int y = oy - stem_half_h; y <= oy + stem_half_h; y++)
            board_lcd_set_pixel_raw(x, y, color);
}

static void draw_arrow_up(int ox, int oy, uint16_t color)
{
    const int tip_y       = oy - 75;
    const int head_base_y = oy;
    const int head_half_w = 45;
    const int stem_half_w = 10;
    const int stem_base_y = oy + 70;

    for (int y = tip_y; y <= head_base_y; y++) {
        int half = ((y - tip_y) * head_half_w) / (head_base_y - tip_y);
        for (int x = ox - half; x <= ox + half; x++)
            board_lcd_set_pixel_raw(x, y, color);
    }
    for (int y = head_base_y; y < stem_base_y; y++)
        for (int x = ox - stem_half_w; x <= ox + stem_half_w; x++)
            board_lcd_set_pixel_raw(x, y, color);
}

void board_lcd_sanity_test(void)
{
    if (!s_panel || !s_fb) {
        ESP_LOGW(TAG, "Panel missing; skip sanity test");
        return;
    }

    uint16_t white  = board_lcd_pack_rgb(255, 255, 255);
    uint16_t yellow = board_lcd_pack_rgb(255, 255,   0);

    board_lcd_clear();
    draw_arrow_up(80,  LCD_V_RES / 2, white);
    draw_arrow_right(240, LCD_V_RES / 2, white);
    board_lcd_flush();

    bool was_touching = false;

    while (1) {
        if (!s_touch) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        esp_lcd_touch_read_data(s_touch);

        esp_lcd_touch_point_data_t pts[1] = {0};
        uint8_t count = 0;
        esp_err_t err = esp_lcd_touch_get_data(s_touch, pts, &count, 1);
        bool pressed = (err == ESP_OK) && (count > 0);

        if (pressed) {
            int sx = pts[0].x;
            int sy = pts[0].y;
            ESP_LOGI(TAG, "touch x=%d y=%d", sx, sy);

            board_lcd_clear();
            draw_arrow_up(80,  LCD_V_RES / 2, white);
            draw_arrow_right(240, LCD_V_RES / 2, white);

            for (int d = -6; d <= 6; d++) {
                if (sx+d >= 0 && sx+d < LCD_H_RES)
                    board_lcd_set_pixel_raw(sx+d, sy, yellow);
                if (sy+d >= 0 && sy+d < LCD_V_RES)
                    board_lcd_set_pixel_raw(sx, sy+d, yellow);
            }

            char buf[24];
            snprintf(buf, sizeof(buf), "X:%d Y:%d", sx, sy);
            draw_string_scaled(8, LCD_V_RES - 29, buf, yellow, 3);

            // LED: green while touched
            if (s_led) {
                led_strip_set_pixel(s_led, 0, 0, 32, 0);
                led_strip_refresh(s_led);
            }

            board_lcd_flush();
            was_touching = true;
        } else if (was_touching) {
            ESP_LOGI(TAG, "touch released");
            board_lcd_clear();
            draw_arrow_up(80,  LCD_V_RES / 2, white);
            draw_arrow_right(240, LCD_V_RES / 2, white);
            board_lcd_flush();

            if (s_led) led_strip_clear(s_led);

            was_touching = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---------------------------------------------------------------------------
// Display drawing API
// ---------------------------------------------------------------------------

int board_lcd_width(void)  { return LCD_H_RES; }
int board_lcd_height(void) { return LCD_V_RES; }

void board_lcd_flush(void)
{
    if (!s_panel || !s_fb) return;
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);
}

void board_lcd_flush_region(int x1, int y1, int x2, int y2)
{
    if (!s_panel || !s_fb) return;
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

void board_lcd_fill(uint16_t color)
{
    if (!s_panel || !s_fb) return;
    uint16_t c = swap_bytes(color);
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++)
        s_fb[i] = c;
    board_lcd_flush();
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
    color = swap_bytes(color);
    *r = ((color >> 11) & 0x1F) << 3;
    *g = ((color >>  5) & 0x3F) << 2;
    *b = ( color        & 0x1F) << 3;
}
