// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#define RADIO_DASH_URL_BYTES 2048U
#define RADIO_DASH_MAX_SEGMENTS 64U

enum radio_dash_result_t {
    RADIO_DASH_OK = 0,
    RADIO_DASH_INVALID = -1,
    RADIO_DASH_MALFORMED = -2,
    RADIO_DASH_UNSUPPORTED = -3,
    RADIO_DASH_LIMIT = -4
};

struct radio_dash_manifest_t {
    char initialization_url[RADIO_DASH_URL_BYTES];
    char segment_urls[RADIO_DASH_MAX_SEGMENTS][RADIO_DASH_URL_BYTES];
    uint32_t segment_count;
};

struct radio_dash_aac_config_t {
    uint32_t sample_rate;
    uint8_t frequency_index;
    uint8_t channel_configuration;
    uint8_t object_type;
};

radio_dash_result_t radio_dash_parse_mpd(const char *data, size_t size,
                                         const char *manifest_url,
                                         radio_dash_manifest_t *manifest);
radio_dash_result_t radio_dash_parse_aac_init(const uint8_t *data, size_t size,
                                              radio_dash_aac_config_t *config);
radio_dash_result_t radio_dash_fragment_to_adts(const uint8_t *fragment, size_t fragment_size,
                                                const radio_dash_aac_config_t *config,
                                                uint8_t *output, size_t output_capacity,
                                                size_t *output_size);
