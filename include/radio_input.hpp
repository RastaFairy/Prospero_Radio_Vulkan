// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>

enum radio_input_key_t
{
    RADIO_INPUT_CROSS,
    RADIO_INPUT_CIRCLE,
    RADIO_INPUT_SQUARE,
    RADIO_INPUT_TRIANGLE,
    RADIO_INPUT_OPTIONS,
    RADIO_INPUT_L1,
    RADIO_INPUT_R1,
    RADIO_INPUT_UP,
    RADIO_INPUT_DOWN,
    RADIO_INPUT_LEFT,
    RADIO_INPUT_RIGHT,
    RADIO_INPUT_VOLUME_UP,
    RADIO_INPUT_VOLUME_DOWN,
    RADIO_INPUT_STATION_PREVIOUS,
    RADIO_INPUT_STATION_NEXT,
    RADIO_INPUT_PAD_CLICK,
    RADIO_INPUT_COUNT
};

struct radio_input_event_t
{
    radio_input_key_t key;
    bool pressed;
};

bool radio_input_init(void);
void radio_input_poll(void);
bool radio_input_next(radio_input_event_t *event);
bool radio_input_pressed(radio_input_key_t key);
void radio_input_shutdown(void);
/* v027: touchpad state read from the pad report and lightbar color.
 * Coordinates are 0..1919 x 0..1087; touch_down false means no finger.
 * Both are best-effort: invalid report layouts leave them inert. */
bool radio_input_touch(unsigned short *x, unsigned short *y);
void radio_input_lightbar(int r, int g, int b);
unsigned long long radio_input_milliseconds(void);
