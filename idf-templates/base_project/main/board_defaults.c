// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

#include "board_interface.h"

// Default no-op for boards without an LCD.
// Boards that have an LCD should override this with a real implementation.
__attribute__((weak)) void board_lcd_sanity_test(void) {}
__attribute__((weak)) void board_lcd_fill(uint16_t color) { (void)color; }
