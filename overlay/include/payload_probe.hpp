// Prospero Radio - local payload persistence bridge.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

#include "radio_disc_protocol.h"
#include "radio_usb_protocol.h"

enum radio_payload_file_t
{
    RADIO_PAYLOAD_CATALOG = 1,
    RADIO_PAYLOAD_FAVORITES = 2,
    RADIO_PAYLOAD_EQ = 3,
    RADIO_PAYLOAD_PRESETS = 4,
    RADIO_PAYLOAD_REPORT = 5,
    RADIO_PAYLOAD_AUXLIST = 6,
    RADIO_PAYLOAD_AUXFAVORITES = 7
};

enum radio_aux_playlist_format_t
{
    RADIO_AUX_FORMAT_AUTO = 0,
    RADIO_AUX_FORMAT_M3U = 1,
    RADIO_AUX_FORMAT_M3U8 = 2,
    RADIO_AUX_FORMAT_PLS = 3,
    RADIO_AUX_FORMAT_XSPF = 4,
    RADIO_AUX_FORMAT_ASX = 5
};

#define RADIO_AUX_PLAYLIST_MAX_BYTES (4U * 1024U * 1024U)
#define RADIO_AUX_PLAYLIST_ENVELOPE_BYTES 9U

// Starts the bundled payload through elfldr and verifies its loopback service.
bool radio_payload_bridge_start();
// Restore a persistent file into its existing /download0 working path. If the
// persistent copy is not present yet, seed it from the local copy.
bool radio_payload_bridge_sync(radio_payload_file_t file);
bool radio_payload_bridge_push(radio_payload_file_t file);
bool radio_payload_bridge_push_file(radio_payload_file_t file, const char *local_path);
bool radio_payload_bridge_pull_report();
bool radio_payload_bridge_keepalive();
// AUX is started only while its UI surface is active; the payload owns port 7000.
bool radio_payload_bridge_aux_start();
bool radio_payload_bridge_aux_stop();
bool radio_payload_bridge_aux_running();
// Move the AUX document directly between /data/radio and caller-owned memory.
bool radio_payload_bridge_fetch_aux_buffer(void *buffer, std::size_t capacity, std::size_t *size,
                                           unsigned char *format, bool *not_found);
bool radio_payload_bridge_store_aux_buffer(const void *buffer, std::size_t size,
                                           unsigned char format);
// Read-only snapshot of the inserted USB optical disc. Audio is streamed
// separately over loopback HTTP and is never copied to a persistent file.
bool radio_payload_bridge_fetch_disc_index(void *buffer, std::size_t capacity,
                                           std::size_t *size, bool force_refresh = false);
// Read-only index of files exposed by system-mounted USB volumes.
bool radio_payload_bridge_fetch_usb_index(void *buffer, std::size_t capacity,
                                          std::size_t *size, bool force_refresh = false);
void radio_payload_bridge_shutdown();
