// Prospero Radio - local payload persistence bridge.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
// Pull the latest M3U from /data/radio into /download0 for the app to process.
bool radio_payload_bridge_fetch_aux();
void radio_payload_bridge_shutdown();
