/**
 * @file      amoled_driver.c
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2024  Shenzhen Xinyuan Electronic Technology Co., Ltd
 * @date      2024-01-07
 *
 */
#include <sdkconfig.h>
#include <driver/spi_master.h>
#include <sys/cdefs.h>
#include "driver/gpio.h"
#include "product_pins.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "board_interface.h"
#include "i2c_driver.h"
#include "power_driver.h"
#include "touch_min.h"

#if defined(CONFIG_LILYGO_T_AMOLED_LITE_147) || \
    defined(CONFIG_LILYGO_T_DISPLAY_S3_AMOLED) || \
    defined(CONFIG_LILYGO_T_DISPLAY_S3_AMOLED_TOUCH) || \
    defined(CONFIG_LILYGO_T4_S3_241)

#define SEND_BUF_SIZE           (16384)
#define DEFAULT_SPI_HANDLER     (SPI3_HOST)

static const char *TAG = "AMOLED";
static uint16_t *pBuffer = NULL;
static spi_device_handle_t spi = NULL;
static uint8_t _brightness;

static void display_init(void);
static uint16_t amoled_width(void);
static uint16_t amoled_height(void);
static void amoled_set_window(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye);
static void amoled_push_buffer(uint16_t *data, uint32_t len);
static void display_push_colors(uint16_t x, uint16_t y, uint16_t width, uint16_t hight, uint16_t *data);

#ifndef LOW
#define LOW 0
#endif  // panel driver block

// --- idf-new board_interface shim ---
const char *board_get_name(void)
{
    return "LilyGO T4 S3 AMOLED Touch 2.41\"";
}

bool board_has_lcd(void)
{
    return true;
}

void board_init(void)
{
    ESP_ERROR_CHECK(i2c_driver_init());
    touch_min_init();
    if (!power_driver_init()) {
        ESP_LOGW(TAG, "Power init failed, continuing");
    }
    display_init();

    // amoled_height() = 450 (columns), amoled_width() = 600 (rows)
    const int w = amoled_height();
    const int h = amoled_width();
    s_fb = heap_caps_aligned_calloc(4, w * h * sizeof(uint16_t), 1,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_fb);
}

static uint16_t *s_fb = NULL;

// Byte-swap RGB565 for SPI wire order (little-endian ESP32 -> big-endian display)
static inline uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

static inline uint16_t pack_rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);
}

// Fill screen using full-frame PSRAM push (row-by-row does not work on this panel)
static void fill_screen(uint16_t color)
{
    // AMOLED_HEIGHT=450 is physical width, AMOLED_WIDTH=600 is physical height
    const int w = amoled_height();  // 450 columns
    const int h = amoled_width();   // 600 rows
    const int total = w * h;
    uint16_t panel_color = swap16(color);

    uint16_t *fb = (uint16_t *)heap_caps_malloc(total * sizeof(uint16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGE(TAG, "fill_screen: PSRAM alloc failed");
        return;
    }
    for (int i = 0; i < total; i++) fb[i] = panel_color;
    display_push_colors(0, 0, w, h, fb);
    heap_caps_free(fb);
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
    ESP_LOGI(TAG, "Starting LCD sanity task...");
    xTaskCreate(lcd_sanity_task, "lcd_sanity", 4096, NULL, 4, NULL);
}

// --- Display drawing API ---
// Logical dimensions: 450 wide x 600 tall (amoled_height x amoled_width).
// Framebuffer stores byte-swapped RGB565 matching the SPI wire format.

int board_lcd_width(void) { return amoled_height(); }   // 450
int board_lcd_height(void) { return amoled_width(); }    // 600

void board_lcd_flush(void)
{
    if (!s_fb) return;
    const int w = amoled_height();
    const int h = amoled_width();
    display_push_colors(0, 0, w, h, s_fb);
}

void board_lcd_clear(void)
{
    if (!s_fb) return;
    memset(s_fb, 0, amoled_height() * amoled_width() * sizeof(uint16_t));
}

void board_lcd_set_pixel_raw(int x, int y, uint16_t color)
{
    if (s_fb) s_fb[y * amoled_height() + x] = color;
}

void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_fb) s_fb[y * amoled_height() + x] = pack_rgb565_swapped(r, g, b);
}

uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return pack_rgb565_swapped(r, g, b);
}

uint16_t board_lcd_get_pixel_raw(int x, int y)
{
    return s_fb ? s_fb[y * amoled_height() + x] : 0;
}

void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    color = (color >> 8) | (color << 8);
    *r = ((color >> 11) & 0x1F) << 3;
    *g = ((color >> 5) & 0x3F) << 2;
    *b = (color & 0x1F) << 3;
}
#ifndef HIGH
#define HIGH 1
#endif
#ifndef OUTPUT
#define OUTPUT GPIO_MODE_OUTPUT
#endif
#ifndef INPUT
#define INPUT   GPIO_MODE_INPUT
#endif


void amoled_write_cmd(uint32_t cmd, uint8_t *pdat, uint32_t lenght);

static bool __init_qspi_bus();

#define delay(ms)   vTaskDelay(ms / portTICK_PERIOD_MS)

static void pinMode(uint32_t gpio, uint8_t mode)
{
    gpio_config_t config = {0};
    config.pin_bit_mask = 1ULL << gpio;
    config.mode = mode == INPUT  ? GPIO_MODE_INPUT : mode == OUTPUT ? GPIO_MODE_OUTPUT : GPIO_MODE_DISABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
}

void digitalWrite(uint32_t gpio, uint8_t level)
{
    gpio_set_level((gpio_num_t )gpio, level);
}

static void inline setCS()
{
    digitalWrite(BOARD_DISP_CS, LOW);
}

static void inline clrCS()
{
    digitalWrite(BOARD_DISP_CS, HIGH);
}

void display_init()
{
    __init_qspi_bus();
}

static bool __init_qspi_bus()
{
#if CONFIG_LILYGO_T_AMOLED_LITE_147
    pBuffer = (uint16_t *)heap_caps_malloc(AMOLED_WIDTH * AMOLED_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!pBuffer) {
        ESP_LOGE(TAG, "ERROR:No memory use .."); return false;
    }
#endif

#if defined(CONFIG_LILYGO_T_AMOLED_LITE_147)
    ESP_LOGI(TAG, "============LILYGO_T_AMOLED_LITE_147============");
#elif defined(CONFIG_LILYGO_T_DISPLAY_S3_AMOLED)
    ESP_LOGI(TAG, "============LILYGO_T_DISPLAY_S3_AMOLED============");
#elif defined(CONFIG_LILYGO_T_DISPLAY_S3_AMOLED_TOUCH)
    ESP_LOGI(TAG, "============T_DISPLAY_S3_AMOLED_TOUCH============");
#elif defined(CONFIG_LILYGO_T4_S3_241)
    ESP_LOGI(TAG, "============LILYGO_T4_S3_241============");
#endif

    ESP_LOGI(TAG, "=====CONFIGURE======");
    ESP_LOGI(TAG, "RST    > %d", BOARD_DISP_RESET);
    ESP_LOGI(TAG, "CS     > %d", BOARD_DISP_CS);
    ESP_LOGI(TAG, "SCK    > %d", BOARD_DISP_SCK);
    ESP_LOGI(TAG, "D0     > %d", BOARD_DISP_DATA0);
    ESP_LOGI(TAG, "D1     > %d", BOARD_DISP_DATA1);
    ESP_LOGI(TAG, "D2     > %d", BOARD_DISP_DATA2);
    ESP_LOGI(TAG, "D3     > %d", BOARD_DISP_DATA3);
    ESP_LOGI(TAG, "TE     > %d", BOARD_DISP_TE);
    ESP_LOGI(TAG, "Freq   > %d", DEFAULT_SCK_SPEED);
    ESP_LOGI(TAG, "Power  > %d", AMOLED_EN_PIN);
    ESP_LOGI(TAG, "==================");

    pinMode(BOARD_DISP_RESET, OUTPUT);
    pinMode(BOARD_DISP_CS, OUTPUT);

    if (BOARD_DISP_TE != -1) {
        //
    }

    if (AMOLED_EN_PIN != -1) {
        pinMode(AMOLED_EN_PIN, OUTPUT);
        digitalWrite(AMOLED_EN_PIN, HIGH);
    }

    //reset display
    digitalWrite(BOARD_DISP_RESET, HIGH);
    delay(200);
    digitalWrite(BOARD_DISP_RESET, LOW);
    delay(300);
    digitalWrite(BOARD_DISP_RESET, HIGH);
    delay(200);

    spi_bus_config_t buscfg = {
        .data0_io_num = BOARD_DISP_DATA0,
        .data1_io_num = BOARD_DISP_DATA1,
        .sclk_io_num = BOARD_DISP_SCK,
        .data2_io_num = BOARD_DISP_DATA2,
        .data3_io_num = BOARD_DISP_DATA3,
        .data4_io_num = BOARD_NONE_PIN,
        .data5_io_num = BOARD_NONE_PIN,
        .data6_io_num = BOARD_NONE_PIN,
        .data7_io_num = BOARD_NONE_PIN,
        .max_transfer_sz = (SEND_BUF_SIZE * 16) + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };

    spi_device_interface_config_t devcfg = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = 0,
        .clock_speed_hz = DEFAULT_SCK_SPEED,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 17,
    };
    esp_err_t ret = spi_bus_initialize(DEFAULT_SPI_HANDLER, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize fail!");
        return false;
    }
    ret = spi_bus_add_device(DEFAULT_SPI_HANDLER, &devcfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device fail!");
        return false;
    }
    // prevent initialization failure
    int retry = 2;
    while (retry--) {
        const lcd_cmd_t *t = AMOLED_INIT_CMD;
        for (uint32_t i = 0; i < AMOLED_INIT_CMD_LEN; i++) {
            amoled_write_cmd(t[i].addr, (uint8_t *)t[i].param, t[i].len & 0x1F);
            if (t[i].len & 0x80) {
                delay(120);
            }
            if (t[i].len & 0x20) {
                delay(10);
            }
        }
    }
    return true;
}

uint16_t  amoled_width()
{
    return AMOLED_WIDTH;
}

uint16_t  amoled_height()
{
    return AMOLED_HEIGHT;
}

void amoled_write_cmd(uint32_t cmd, uint8_t *pdat, uint32_t lenght)
{
    setCS();
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.flags = (SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR);
    t.cmd = 0x02;
    t.addr = cmd;
    if (lenght != 0) {
        t.tx_buffer = pdat;
        t.length = 8 * lenght;
    } else {
        t.tx_buffer = NULL;
        t.length = 0;
    }
    spi_device_polling_transmit(spi, &t);
    clrCS();
}

void amoled_set_brightness(uint8_t level)
{
    _brightness = level;
    lcd_cmd_t t = {0x5100, {level}, 0x01};
    amoled_write_cmd(t.addr, t.param, t.len);
}

uint8_t amoled_get_brightness()
{
    return _brightness;
}

void amoled_set_window(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye)
{
#if CONFIG_LILYGO_T4_S3_241
    xs += 16;
    xe += 16;
#endif

    lcd_cmd_t t[3] = {
        {
            0x2A00, {
                (uint8_t)((xs >> 8) & 0xFF),
                (uint8_t)(xs & 0xFF),
                (uint8_t)((xe >> 8) & 0xFF),
                (uint8_t)(xe & 0xFF)
            }, 0x04
        },
        {
            0x2B00, {
                (uint8_t)((ys >> 8) & 0xFF),
                (uint8_t)(ys & 0xFF),
                (uint8_t)((ye >> 8) & 0xFF),
                (uint8_t)(ye & 0xFF)
            }, 0x04
        },
        {
            0x2C00, {
                0x00
            }, 0x00
        },
    };

    for (uint32_t i = 0; i < 3; i++) {
        amoled_write_cmd(t[i].addr, t[i].param, t[i].len);
    }
}

// Push (aka write pixel) colours to the TFT (use amoled_set_window() first)
void amoled_push_buffer(uint16_t *data, uint32_t len)
{
    bool first_send = true;
    uint16_t *p = data;
    assert(p);
    assert(spi);
    setCS();
    do {
        size_t chunk_size = len;
        spi_transaction_ext_t t = {0};
        memset(&t, 0, sizeof(t));
        if (first_send) {
            t.base.flags = SPI_TRANS_MODE_QIO;
            t.base.cmd = 0x32 ;
            t.base.addr = 0x002C00;
            first_send = 0;
        } else {
            t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
            t.command_bits = 0;
            t.address_bits = 0;
            t.dummy_bits = 0;
        }
        if (chunk_size > SEND_BUF_SIZE) {
            chunk_size = SEND_BUF_SIZE;
        }
        t.base.tx_buffer = p;
        t.base.length = chunk_size * 16;
        spi_device_polling_transmit(spi, (spi_transaction_t *)&t);
        len -= chunk_size;
        p += chunk_size;
    } while (len > 0);
    clrCS();
}

void display_push_colors(uint16_t x, uint16_t y, uint16_t width, uint16_t hight, uint16_t *data)
{

    if (pBuffer) {
        assert(pBuffer);
        uint16_t _x = AMOLED_WIDTH - (y + hight);
        uint16_t _y = x;
        uint16_t _h = width;
        uint16_t _w = hight;
        uint16_t *p = data;
        uint32_t cum = 0;
        for (uint16_t j = 0; j < width; j++) {
            for (uint16_t i = 0; i < hight; i++) {
                pBuffer[cum] = ((uint16_t)p[width * (hight - i - 1) + j]);
                cum++;
            }
        }
        amoled_set_window(_x, _y, _x + _w - 1, _y + _h - 1);
        amoled_push_buffer(pBuffer, width * hight);
    } else {
        amoled_set_window(x, y, x + width - 1, y + hight - 1);
        amoled_push_buffer(data, width * hight);
    }
}


#endif














