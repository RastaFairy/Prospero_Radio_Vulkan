// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

// Decrypts one complete AES-128-CBC HLS media segment in place and validates
// its PKCS#7 padding. Returns 0 on success and leaves *plain_size unchanged on
// failure. The input length must be a non-zero multiple of the AES block size.
int radio_aes128_cbc_decrypt(uint8_t *data, size_t size, const uint8_t key[16],
                             const uint8_t iv[16], size_t *plain_size);
