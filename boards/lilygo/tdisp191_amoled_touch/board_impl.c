// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOARD_NAME "LilyGO T-Display S3 AMOLED Touch 1.91\""

#define LCD_HOST SPI3_HOST
#define LCD_H_RES 536 // panel is mounted landscape (long edge is X)
#define LCD_V_RES 240

#define PIN_LCD_SCK   47
#define PIN_LCD_D0    18
#define PIN_LCD_D1    7
#define PIN_LCD_D2    48
#define PIN_LCD_D3    5
#define PIN_LCD_CS    6
#define PIN_LCD_RST   17
#define PIN_LCD_TE    9
#define PIN_LCD_EN    38

#define LCD_SPI_FREQ_HZ (75 * 1000 * 1000)
#define LCD_SEND_CHUNK_PIXELS 16384

#define PIN_TOUCH_SDA 3
#define PIN_TOUCH_SCL 2
#define PIN_TOUCH_INT 21
#define TOUCH_I2C_PORT I2C_NUM_0

#define TOUCH_POLL_MS 50

typedef struct {
    uint32_t addr;
    uint8_t param[20];
    uint32_t len;
} lcd_cmd_t;

#define AMOLED_DEFAULT_BRIGHTNESS 175

static const lcd_cmd_t s_rm67162_init[] = {
    {0x1100, {0x00}, 0x80},
    {0x3A00, {0x55}, 0x01},
    {0x5100, {0x00}, 0x01},
    {0x2900, {0x00}, 0x80},
    {0x5100, {AMOLED_DEFAULT_BRIGHTNESS}, 0x01},
    {0x3600, {0x60}, 0x01}, // landscape orientation (MX=1, MV=1)
};

static spi_device_handle_t s_spi = NULL;
static bool s_lcd_ready = false;
static esp_lcd_touch_handle_t s_touch = NULL;
static bool s_touch_task_started = false;
static uint16_t s_tx_buf[LCD_SEND_CHUNK_PIXELS];
static uint16_t *s_fb = NULL;
static const char *TAG = "BOARD_TDS3_AMOLED";

// RM67162 expects big-endian RGB565 over QSPI, but ESP32-S3 stores
// little-endian uint16_t.  Byte-swap each pixel before DMA.
static inline uint16_t bswap16(uint16_t v)
{
    return (v >> 8) | (v << 8);
}

static inline void panel_select(void)
{
    gpio_set_level(PIN_LCD_CS, 0);
}

static inline void panel_deselect(void)
{
    gpio_set_level(PIN_LCD_CS, 1);
}

static void amoled_write_cmd(uint32_t cmd, const uint8_t *data, uint32_t length)
{
    spi_transaction_t t = {0};
    t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    t.cmd = 0x02;
    t.addr = cmd;
    t.length = length * 8;
    t.tx_buffer = data;
    panel_select();
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    panel_deselect();
}

static void amoled_set_window(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye)
{
    const uint8_t caset[] = {
        (uint8_t)((xs >> 8) & 0xFF), (uint8_t)(xs & 0xFF),
        (uint8_t)((xe >> 8) & 0xFF), (uint8_t)(xe & 0xFF),
    };
    const uint8_t raset[] = {
        (uint8_t)((ys >> 8) & 0xFF), (uint8_t)(ys & 0xFF),
        (uint8_t)((ye >> 8) & 0xFF), (uint8_t)(ye & 0xFF),
    };

    amoled_write_cmd(0x2A00, caset, sizeof(caset));
    amoled_write_cmd(0x2B00, raset, sizeof(raset));
    amoled_write_cmd(0x2C00, NULL, 0);
}

static void amoled_push_buffer(const uint16_t *data, size_t pixel_count)
{
    if (!s_spi || !pixel_count) {
        return;
    }
    bool first = true;
    size_t remaining = pixel_count;
    const uint16_t *p = data;

    panel_select();
    while (remaining) {
        size_t chunk = remaining;
        if (chunk > LCD_SEND_CHUNK_PIXELS) {
            chunk = LCD_SEND_CHUNK_PIXELS;
        }
        for (size_t i = 0; i < chunk; ++i) {
            s_tx_buf[i] = bswap16(p[i]);
        }

        spi_transaction_ext_t t = {0};
        if (first) {
            t.base.flags = SPI_TRANS_MODE_QIO;
            t.base.cmd = 0x32;
            t.base.addr = 0x002C00;
            first = false;
        } else {
            t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                       SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
        }
        t.base.tx_buffer = s_tx_buf;
        t.base.length = chunk * 16;
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
        p += chunk;
        remaining -= chunk;
    }
    panel_deselect();
}

static void panel_reset(void)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t init_display(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    gpio_set_level(PIN_LCD_CS, 1);

    if (PIN_LCD_EN >= 0) {
        gpio_config_t en_cfg = out_cfg;
        en_cfg.pin_bit_mask = 1ULL << PIN_LCD_EN;
        ESP_ERROR_CHECK(gpio_config(&en_cfg));
        gpio_set_level(PIN_LCD_EN, 1);
    }

    panel_reset();

    spi_bus_config_t buscfg = {
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .sclk_io_num = PIN_LCD_SCK,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = (LCD_SEND_CHUNK_PIXELS * 16) + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };

    spi_device_interface_config_t devcfg = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = 0,
        .clock_speed_hz = LCD_SPI_FREQ_HZ,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 10,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &s_spi));

    for (int retry = 0; retry < 2; ++retry) {
        for (size_t i = 0; i < (sizeof(s_rm67162_init) / sizeof(s_rm67162_init[0])); ++i) {
            const lcd_cmd_t *cmd = &s_rm67162_init[i];
            uint32_t payload = cmd->len & 0x1F;
            if (payload > 0) {
                amoled_write_cmd(cmd->addr, cmd->param, payload);
            } else {
                amoled_write_cmd(cmd->addr, NULL, 0);
            }
            if (cmd->len & 0x80) {
                vTaskDelay(pdMS_TO_TICKS(120));
            } else if (cmd->len & 0x20) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    s_lcd_ready = true;
    ESP_LOGI(TAG, "AMOLED ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

static void fill_screen(uint16_t color)
{
    if (!s_lcd_ready) {
        return;
    }

    static uint16_t line[LCD_H_RES];
    for (int x = 0; x < LCD_H_RES; ++x) {
        line[x] = color;
    }

    for (int y = 0; y < LCD_V_RES; ++y) {
        amoled_set_window(0, y, LCD_H_RES - 1, y);
        amoled_push_buffer(line, LCD_H_RES);
    }
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
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

static void start_touch_logger(void)
{
    if (s_touch_task_started) {
        return;
    }
    BaseType_t ok = xTaskCreate(touch_logger_task, "TouchLoggerAmoled", 2048, NULL, 5, NULL);
    if (ok == pdPASS) {
        s_touch_task_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to start touch logger task");
    }
}

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
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = 0; // legacy driver expects bus speed from I2C driver
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_I2C_PORT, &io_cfg, &tp_io));

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = PIN_TOUCH_INT,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io, &touch_cfg, &s_touch));
    start_touch_logger();
}

void board_init(void)
{
    ESP_LOGI(TAG, "%s init", BOARD_NAME);
    ESP_ERROR_CHECK(init_display());
    init_touch();

    s_fb = heap_caps_aligned_calloc(4, LCD_H_RES * LCD_V_RES * sizeof(uint16_t), 1,
                MALLOC_CAP_DEFAULT);
    assert(s_fb);
}

const char *board_get_name(void)
{
    return BOARD_NAME;
}

bool board_has_lcd(void)
{
    return s_lcd_ready;
}

void board_lcd_sanity_test(void)
{
    if (!s_lcd_ready) {
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
// Framebuffer stores standard RGB565. The byte-swap to big-endian
// (required by the RM67162 QSPI interface) is applied during flush
// inside amoled_push_buffer().

int board_lcd_width(void) { return LCD_H_RES; }
int board_lcd_height(void) { return LCD_V_RES; }

void board_lcd_flush(void)
{
    if (!s_lcd_ready || !s_fb) return;
    amoled_set_window(0, 0, LCD_H_RES - 1, LCD_V_RES - 1);
    amoled_push_buffer(s_fb, LCD_H_RES * LCD_V_RES);
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
