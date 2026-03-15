// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// MSM261S4030H0R I2S digital microphone (BCK=IO15, WS=IO2, SD=IO39, EN=IO12).
// Outputs right channel (L/R tied high). Sample rate configurable via menuconfig.
// Returns 16-bit PCM samples converted from 32-bit I2S frames.

esp_err_t mic_init(void);

// Read PCM samples. Blocks until num_samples are available.
esp_err_t mic_read(int16_t *samples, size_t num_samples, size_t *read_count);
