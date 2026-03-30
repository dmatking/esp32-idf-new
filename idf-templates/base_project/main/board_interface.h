// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <stdbool.h>
#include <stdint.h>

void board_init(void);
const char *board_get_name(void);
bool board_has_lcd(void);

// Optional: implement to visually verify the LCD on startup.
// A default no-op is provided by board_defaults.c for headless boards.
void board_lcd_sanity_test(void);

// Fill the LCD with a RGB565 color. No-op if no LCD.
void board_lcd_fill(uint16_t color);

// ---------------------------------------------------------------------------
// Display drawing API — framebuffer-based pixel access for graphics demos.
// Boards with an LCD should implement these. Weak no-op defaults are provided
// in board_defaults.c for headless boards.
// ---------------------------------------------------------------------------

// Display dimensions in pixels.
int board_lcd_width(void);
int board_lcd_height(void);

// Push the framebuffer contents to the display. Blocks until complete.
void board_lcd_flush(void);

// Clear the framebuffer to black.
void board_lcd_clear(void);

// Write a pre-packed pixel value (native format, already byte-swapped).
void board_lcd_set_pixel_raw(int x, int y, uint16_t color);

// Write an RGB888 pixel, converting to the display's native format.
void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b);

// Convert RGB888 to the display's native packed pixel format (RGB565).
uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b);

// Read a raw pixel value back from the framebuffer.
uint16_t board_lcd_get_pixel_raw(int x, int y);

// Extract RGB888 components from a raw pixel value.
void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b);
