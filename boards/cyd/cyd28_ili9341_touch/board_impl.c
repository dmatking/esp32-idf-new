// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOARD_NAME "CYD 2.8\" ILI9341 (E32R28T/E32N28T)"

// --- LCD (ILI9341) on SPI2_HOST (HSPI) ---
#define LCD_HOST     SPI2_HOST
#define LCD_H_RES    320  // landscape
#define LCD_V_RES    240
#define PIN_LCD_SCLK 14
#define PIN_LCD_MOSI 13
#define PIN_LCD_MISO 12
#define PIN_LCD_DC    2
#define PIN_LCD_CS   15
#define PIN_LCD_RST  -1
#define PIN_LCD_BL   21  // high = backlight on (via BSS138 N-FET)

// --- Touch (XPT2046) on SPI3_HOST (VSPI) ---
#define TOUCH_HOST     SPI3_HOST
#define PIN_TOUCH_CLK  25
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39  // input-only GPIO
#define PIN_TOUCH_CS   33
#define PIN_TOUCH_IRQ  36  // input-only GPIO, low = touched

// XPT2046 control bytes: start=1, channel, 12-bit, differential, PD=00
// PD=00 = power-down between conversions; re-enables PENIRQ after each read.
// PD=11 (0xD3/0x93) would disable PENIRQ and our IRQ-based detection breaks.
#define XPT_CMD_X  0xD0   // channel X+, diff, power-down/PENIRQ enabled
#define XPT_CMD_Y  0x90   // channel Y+, diff, power-down/PENIRQ enabled

// --- RGB LED: common anode, active LOW ---
#define PIN_LED_R 17
#define PIN_LED_G 22
#define PIN_LED_B 16

static esp_lcd_panel_handle_t s_panel = NULL;
static spi_device_handle_t    s_touch_spi = NULL;
static uint16_t              *s_fb = NULL;
static SemaphoreHandle_t      s_flush_sem = NULL;
static const char             *TAG = "BOARD_CYD28";

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
// RGB LED
// ---------------------------------------------------------------------------

static void init_rgb_led(void)
{
    // Common-anode: high = off, low = on.
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_R) | (1ULL << PIN_LED_G) | (1ULL << PIN_LED_B),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_LED_R, 1);
    gpio_set_level(PIN_LED_G, 1);
    gpio_set_level(PIN_LED_B, 1);
}

// ---------------------------------------------------------------------------
// XPT2046 touch
// ---------------------------------------------------------------------------

// Read one 12-bit ADC sample from the XPT2046 in a single 3-byte SPI frame.
static uint16_t xpt2046_read_raw(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0x00, 0x00, 0x00};
    spi_transaction_t t = {
        .length = 24,       // bits
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(s_touch_spi, &t);
    // Result: 1 null bit after cmd byte, then 12 data bits.
    // rx[1] bits[6:0] = D11:D5, rx[2] bits[7:3] = D4:D0.
    return (((uint16_t)(rx[1] & 0x7F)) << 5) | (rx[2] >> 3);
}

static bool xpt2046_is_touched(void)
{
    return gpio_get_level(PIN_TOUCH_IRQ) == 0;
}

static void init_touch(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_TOUCH_CLK,
        .mosi_io_num = PIN_TOUCH_MOSI,
        .miso_io_num = PIN_TOUCH_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 6,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_HOST, &bus, SPI_DMA_DISABLED));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_TOUCH_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(TOUCH_HOST, &dev, &s_touch_spi));

    // IRQ pin: input-only, no pull needed (hardware pull-up R22 on board).
    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << PIN_TOUCH_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&irq_cfg);

    // Send one PD=00 conversion to re-enable PENIRQ in case a previous
    // session left the XPT2046 in PD=11 (PENIRQ disabled) state.
    xpt2046_read_raw(XPT_CMD_X);
}

// ---------------------------------------------------------------------------
// Touch calibration — raw ADC at each screen edge in landscape/USB-right.
// Adjust these after tapping corners and reading serial "raw_x= raw_y=" logs.
// Mapping: raw_y → screen X,  raw_x (inverted) → screen Y.
// ---------------------------------------------------------------------------
#define CAL_SX0  200    // raw_y at left edge  (screen x=0)
#define CAL_SX1  3800   // raw_y at right edge (screen x=319)
#define CAL_SY0  200    // raw_x at top edge   (screen y=0)
#define CAL_SY1  3800   // raw_x at bottom edge(screen y=239)

static void touch_to_screen(uint16_t raw_x, uint16_t raw_y, int *sx, int *sy)
{
    *sx = ((int)raw_y - CAL_SX0) * (LCD_H_RES - 1) / (CAL_SX1 - CAL_SX0);
    *sy = ((int)CAL_SY0 - raw_x) * (LCD_V_RES - 1) / (CAL_SY0 - CAL_SY1);
    if (*sx < 0) *sx = 0; else if (*sx >= LCD_H_RES) *sx = LCD_H_RES - 1;
    if (*sy < 0) *sy = 0; else if (*sy >= LCD_V_RES) *sy = LCD_V_RES - 1;
}

// ---------------------------------------------------------------------------
// Minimal 5×7 bitmap font (column-major, bit 0 = top row).
// ---------------------------------------------------------------------------
#define FONT_W  5
#define FONT_H  7
#define FONT_GAP 1   // pixels between characters

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
    return s_glyphs[0].cols; // fallback: space
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
    ESP_LOGI(TAG, "%s init: ILI9341 240x320 + XPT2046 touch", BOARD_NAME);
    init_backlight();
    init_rgb_led();

    // LCD SPI bus (SPI2 / HSPI — native ILI9341 pins on the CYD).
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
        // ILI9341 panels have BGR physical connections; this sets MADCTL BGR=1
        // so the panel correctly interprets our RGB565 pixel data.
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // Landscape, USB connector on right.
    // With MV=1 (swap_xy), MY controls perceived-X and MX controls perceived-Y.
    // MY=0, MX=0 gives correct un-inverted X and Y in landscape.
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
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
// Sanity test — right arrow + live touch coordinate display
// ---------------------------------------------------------------------------

static void draw_arrow_right(int ox, int oy, uint16_t color)
{
    // Right-pointing arrow, origin (ox,oy) = centre of the arrow body.
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
    // Up-pointing arrow, origin (ox,oy) = centre of the arrow body.
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
    draw_arrow_up(80,  LCD_V_RES / 2, white);   // left half
    draw_arrow_right(240, LCD_V_RES / 2, white); // right half
    board_lcd_flush();

    bool was_touching = false;

    while (1) {
        if (!s_touch_spi) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        if (xpt2046_is_touched()) {
            uint32_t sum_x = 0, sum_y = 0;
            for (int i = 0; i < 4; i++) {
                sum_x += xpt2046_read_raw(XPT_CMD_X);
                sum_y += xpt2046_read_raw(XPT_CMD_Y);
            }
            uint16_t raw_x = sum_x / 4;
            uint16_t raw_y = sum_y / 4;

            int sx, sy;
            touch_to_screen(raw_x, raw_y, &sx, &sy);

            ESP_LOGI(TAG, "touch raw_x=%u raw_y=%u  screen x=%d y=%d",
                     raw_x, raw_y, sx, sy);

            board_lcd_clear();
            draw_arrow_up(80,  LCD_V_RES / 2, white);
            draw_arrow_right(240, LCD_V_RES / 2, white);

            // Crosshair at touch point
            for (int d = -6; d <= 6; d++) {
                if (sx+d >= 0 && sx+d < LCD_H_RES)
                    board_lcd_set_pixel_raw(sx+d, sy, yellow);
                if (sy+d >= 0 && sy+d < LCD_V_RES)
                    board_lcd_set_pixel_raw(sx, sy+d, yellow);
            }

            // Coordinate label at bottom, 3× scale (~21px tall)
            char buf[24];
            snprintf(buf, sizeof(buf), "X:%d Y:%d", sx, sy);
            draw_string_scaled(8, LCD_V_RES - 29, buf, yellow, 3);

            board_lcd_flush();
            was_touching = true;
        } else if (was_touching) {
            ESP_LOGI(TAG, "touch released");
            board_lcd_clear();
            draw_arrow_up(80,  LCD_V_RES / 2, white);
            draw_arrow_right(240, LCD_V_RES / 2, white);
            board_lcd_flush();
            was_touching = false;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
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

void board_lcd_fill(uint16_t color)
{
    if (!s_panel || !s_fb) return;
    uint16_t c = swap_bytes(color);
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        s_fb[i] = c;
    }
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
