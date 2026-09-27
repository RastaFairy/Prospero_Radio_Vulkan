// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "radio_input.hpp"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define INPUT_QUEUE_SIZE 64U
#define PAD_SAMPLE_SIZE 120U
#define PAD_SAMPLE_CAPACITY 64
#define PAD_BUTTON_INTERCEPTED UINT32_C(0x80000000)
#define STICK_LOW 64U
#define STICK_HIGH 192U
#define STICK_REPEAT_DELAY_MS UINT64_C(350)
#define STICK_REPEAT_MS UINT64_C(140)
#define TUNING_REPEAT_DELAY_MS UINT64_C(420)
#define TUNING_REPEAT_MS UINT64_C(260)
#define STICK_ROTARY_DEADZONE_RADIUS 46.0f
#define VOLUME_STEP_DEG 14.4f
#define VOLUME_STEP_INTERVAL_MS UINT64_C(60)
#define TUNING_STEP_DEG 45.0f
#define TUNING_STEP_INTERVAL_MS UINT64_C(120)

struct button_map_t
{
    uint32_t button;
    radio_input_key_t key;
};

extern "C"
{
    extern int scePadInit(void);
    extern int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *param);
    extern int scePadClose(int32_t handle);
    extern int scePadRead(int32_t handle, void *data, int32_t num);
    extern int sceUserServiceInitialize(void *init_params);
    extern int sceUserServiceGetInitialUser(int32_t *user_id);
    extern int sceUserServiceTerminate(void);
    extern uint64_t SDL_GetTicks64(void);
    extern int scePadSetLightBar(int32_t handle, const void *param);
}

static const button_map_t buttons[] = {
    {UINT32_C(0x00004000), RADIO_INPUT_CROSS},   {UINT32_C(0x00002000), RADIO_INPUT_CIRCLE},
    {UINT32_C(0x00008000), RADIO_INPUT_SQUARE},  {UINT32_C(0x00001000), RADIO_INPUT_TRIANGLE},
    {UINT32_C(0x00000008), RADIO_INPUT_OPTIONS}, {UINT32_C(0x00000400), RADIO_INPUT_L1},
    {UINT32_C(0x00000800), RADIO_INPUT_R1},      {UINT32_C(0x00000010), RADIO_INPUT_UP},
    {UINT32_C(0x00000040), RADIO_INPUT_DOWN},    {UINT32_C(0x00000080), RADIO_INPUT_LEFT},
    {UINT32_C(0x00000020), RADIO_INPUT_RIGHT},
};

static radio_input_event_t queue[INPUT_QUEUE_SIZE];
static unsigned char samples[PAD_SAMPLE_CAPACITY][PAD_SAMPLE_SIZE];
static unsigned queue_read;
static unsigned queue_write;
static uint32_t button_state;
static float left_stick_angle;
static float left_stick_accum;
static bool left_rotary_active;
static uint64_t left_step_at;
static float right_stick_angle;
static float right_stick_accum;
static bool right_rotary_active;
static uint64_t right_step_at;
static int32_t pad_handle = -1;
static unsigned short touch_x = 0, touch_y = 0;
static bool touch_finger = false;
static bool touch_valid = false;
static bool owns_user_service;

static uint64_t monotonic_milliseconds(void)
{
    return SDL_GetTicks64();
}

static float stick_angle(uint8_t x, uint8_t y, float *radius)
{
    const float dx = (float)x - 128.0f;
    const float dy = (float)y - 128.0f;
    *radius = sqrtf(dx * dx + dy * dy);
    return atan2f(dy, dx) * (180.0f / 3.14159265f);
}

static void queue_push(radio_input_key_t key, bool pressed);

/* Rotary potentiometer emulation: while the stick stays deflected, accumulate
 * the angular travel around its centre; every step_deg of rotation emits one
 * step (clockwise = louder / next station), rate-limited per stick. Dropping
 * below the deadzone resets the origin so each gesture starts where the user
 * grabbed the knob. */
static void rotary_update(uint64_t now, uint64_t step_interval_ms, float angle, float radius,
                          float step_deg, bool *active, float *last_angle, float *accum,
                          uint64_t *step_at, radio_input_key_t clockwise_key,
                          radio_input_key_t anticlockwise_key)
{
    if (radius < STICK_ROTARY_DEADZONE_RADIUS)
    {
        *active = false;
        return;
    }
    if (!*active)
    {
        *active = true;
        *last_angle = angle;
        *accum = 0.0f;
        return;
    }
    float delta = angle - *last_angle;
    if (delta > 180.0f)
        delta -= 360.0f;
    if (delta < -180.0f)
        delta += 360.0f;
    *last_angle = angle;
    if (delta == 0.0f || now < *step_at)
        return;
    *accum += delta;
    if (fabsf(*accum) < step_deg)
        return;
    const int direction = *accum > 0.0f ? 1 : -1;
    *accum -= direction * step_deg;
    *step_at = now + step_interval_ms;
    queue_push(direction > 0 ? clockwise_key : anticlockwise_key, true);
}

static void queue_push(radio_input_key_t key, bool pressed)
{
    const unsigned next = (queue_write + 1U) % INPUT_QUEUE_SIZE;
    if (next == queue_read)
    {
        /* ponytail: bounded input; discard the oldest event instead of allocating. */
        queue_read = (queue_read + 1U) % INPUT_QUEUE_SIZE;
    }
    queue[queue_write] = (radio_input_event_t){key, pressed};
    queue_write = next;
}

static void update_analog_action(int current, int *previous, uint64_t *repeat_at, uint64_t now,
                                 uint64_t repeat_delay)
{
    if (current == *previous)
        return;
    if (*previous >= 0)
        queue_push((radio_input_key_t)*previous, false);
    *previous = current;
    if (current >= 0)
    {
        queue_push((radio_input_key_t)current, true);
        *repeat_at = now + repeat_delay;
    }
}

static void repeat_analog_action(int action, uint64_t *repeat_at, uint64_t now,
                                 uint64_t repeat_period)
{
    if (action < 0 || now < *repeat_at)
        return;
    queue_push((radio_input_key_t)action, true);
    *repeat_at = now + repeat_period;
}

static void process_sample(const unsigned char *sample)
{
    uint32_t current;
    memcpy(&current, sample, sizeof(current));
    const bool neutral = sample[76] == 0 || (current & PAD_BUTTON_INTERCEPTED) != 0;
    if (neutral)
        current = 0;

    const uint32_t changed = button_state ^ current;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
    {
        if ((changed & buttons[i].button) != 0)
            queue_push(buttons[i].key, (current & buttons[i].button) != 0);
    }
    button_state = current;

    /* Touchpad: click is bit 20 of the button mask; the finger block
     * sits at 0x20 (counter u16, status, then id+x+y slots). The
     * validity gate keeps the feature inert on unknown layouts. */
    if ((current & UINT32_C(0x00100000)) != 0)
        queue_push(RADIO_INPUT_PAD_CLICK, true);
    else if (button_state != 0)
        queue_push(RADIO_INPUT_PAD_CLICK, false);
    if (sample[0x20] != 0 || sample[0x21] != 0)
    {
        const unsigned short px = (unsigned short)(sample[0x25] | (sample[0x26] << 8));
        const unsigned short py = (unsigned short)(sample[0x27] | (sample[0x28] << 8));
        if (px < 1920 && py < 1080)
        {
            touch_x = px;
            touch_y = py;
            touch_finger = (sample[0x24] & 0x80) == 0;
            touch_valid = true;
        }
    }

    const uint64_t now = monotonic_milliseconds();
    if (neutral)
    {
        left_rotary_active = false;
        right_rotary_active = false;
    }
    else
    {
        float radius = 0.0f;
        const float left_angle = stick_angle(sample[4], sample[5], &radius);
        rotary_update(now, VOLUME_STEP_INTERVAL_MS, left_angle, radius, VOLUME_STEP_DEG,
                      &left_rotary_active, &left_stick_angle, &left_stick_accum, &left_step_at,
                      RADIO_INPUT_VOLUME_UP, RADIO_INPUT_VOLUME_DOWN);
        const float right_angle = stick_angle(sample[6], sample[7], &radius);
        rotary_update(now, TUNING_STEP_INTERVAL_MS, right_angle, radius, TUNING_STEP_DEG,
                      &right_rotary_active, &right_stick_angle, &right_stick_accum, &right_step_at,
                      RADIO_INPUT_STATION_NEXT, RADIO_INPUT_STATION_PREVIOUS);
    }
}

bool radio_input_init(void)
{
    const int user_init = sceUserServiceInitialize(nullptr);
    owns_user_service = user_init == 0;

    int32_t user_id = -1;
    if (sceUserServiceGetInitialUser(&user_id) < 0 || scePadInit() < 0)
    {
        radio_input_shutdown();
        return false;
    }
    pad_handle = scePadOpen(user_id, 0, 0, nullptr);
    if (pad_handle < 0)
    {
        radio_input_shutdown();
        return false;
    }
    queue_read = queue_write = 0;
    button_state = 0;
    left_stick_angle = left_stick_accum = 0.0f;
    left_rotary_active = false;
    left_step_at = 0;
    right_stick_angle = right_stick_accum = 0.0f;
    right_rotary_active = false;
    right_step_at = 0;
    (void)update_analog_action;
    (void)repeat_analog_action;
    return true;
}

void radio_input_poll(void)
{
    if (pad_handle < 0)
        return;
    const int count = scePadRead(pad_handle, samples, PAD_SAMPLE_CAPACITY);
    for (int i = 0; i < count; ++i)
        process_sample(samples[i]);
}

bool radio_input_next(radio_input_event_t *event)
{
    if (event == nullptr || queue_read == queue_write)
        return false;
    *event = queue[queue_read];
    queue_read = (queue_read + 1U) % INPUT_QUEUE_SIZE;
    return true;
}

bool radio_input_pressed(radio_input_key_t key)
{
    if (key < 0 || key >= RADIO_INPUT_COUNT)
        return false;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
    {
        if (buttons[i].key == key)
            return (button_state & buttons[i].button) != 0;
    }
    return false;
}

unsigned long long radio_input_milliseconds(void)
{
    return SDL_GetTicks64();
}

bool radio_input_touch(unsigned short *x, unsigned short *y)
{
    if (!touch_valid)
        return false;
    if (x)
        *x = touch_x;
    if (y)
        *y = touch_y;
    return touch_finger;
}

void radio_input_lightbar(int r, int g, int b)
{
    if (pad_handle < 0)
        return;
    const unsigned char param[4] = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 0};
    scePadSetLightBar(pad_handle, param);
}

void radio_input_shutdown(void)
{
    if (pad_handle >= 0)
    {
        scePadClose(pad_handle);
        pad_handle = -1;
    }
    if (owns_user_service)
    {
        sceUserServiceTerminate();
        owns_user_service = false;
    }
    queue_read = queue_write = 0;
    button_state = 0;
    left_rotary_active = false;
    right_rotary_active = false;
}
