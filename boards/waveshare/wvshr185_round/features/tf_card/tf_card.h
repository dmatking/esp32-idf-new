// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

// TF (micro-SD) card via SDMMC native 1-bit mode
// Hardware: CLK=IO14, CMD=IO17, D0=IO16
// IO expander: TCA9554PWR at I2C addr 0x20 (SCL=IO10, SDA=IO11)
// The expander holds SD D3/CS HIGH, enabling native SDMMC mode.

#pragma once
#include "esp_err.h"

// Mount the SD card. Returns ESP_OK on success.
// On success, files can be accessed at CONFIG_TF_CARD_MOUNT_POINT.
esp_err_t tf_card_init(void);

// Returns the VFS mount point string (e.g. "/sdcard").
const char *tf_card_get_mount_point(void);
