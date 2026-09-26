# Console UX integration patch for ProsperoRadio Vulkan Edition.
# Applied by overlay/apply-vulkan.py on the materialized upstream worktree:
#   - perceptual (audio-taper) volume curve for sceAudioOut,
#   - rotary potentiometer emulation on both analog sticks,
#   - cabinet-finish theme system with persistence in /download0,
#   - control-atlas wiring (volume frame overlay, tuner highlights)
#     using the spritesheet classes generated from manifest-hybrid.json.
from __future__ import annotations

import json
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Expected exactly one occurrence in {path}, got {count}: {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def patch_volume_taper(src: Path) -> None:
    service = src / "radio_service.cpp"
    replace_once(
        service,
        '#include "radio_service.hpp"',
        '#include "radio_service.hpp"\n\n#include <cmath>',
    )
    replace_once(
        service,
        "    const int level = (AUDIO_OUT_VOLUME_0DB * percent) / 100;",
        "    /* Perceptual audio-taper curve: the console's linear map left the upper\n"
        "     * half of the knob nearly inaudible (50->100 percent is only 6 dB). A\n"
        "     * 2.5-power taper gives the whole travel an even perceived response. */\n"
        "    const float tapered = std::pow(static_cast<float>(percent) / 100.0f, 2.5f);\n"
        "    const int level = static_cast<int>(AUDIO_OUT_VOLUME_0DB * tapered + 0.5f);",
    )


ROTARY_HELPERS = """static float stick_angle(uint8_t x, uint8_t y, float *radius)
{
    const float dx = (float)x - 128.0f;
    const float dy = (float)y - 128.0f;
    *radius = sqrtf(dx * dx + dy * dy);
    return atan2f(dy, dx) * (180.0f / 3.14159265f);
}
"""

ROTARY_UPDATE = """/* Rotary potentiometer emulation: while the stick stays deflected, accumulate
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
"""


def patch_rotary_sticks(src: Path) -> None:
    radio_input = src / "radio_input.cpp"
    replace_once(
        radio_input,
        "#include <string.h>",
        "#include <string.h>\n#include <math.h>",
    )
    replace_once(
        radio_input,
        "#define TUNING_REPEAT_MS UINT64_C(260)",
        "#define TUNING_REPEAT_MS UINT64_C(260)\n"
        "#define STICK_ROTARY_DEADZONE_RADIUS 46.0f\n"
        "#define VOLUME_STEP_DEG 14.4f\n"
        "#define VOLUME_STEP_INTERVAL_MS UINT64_C(60)\n"
        "#define TUNING_STEP_DEG 45.0f\n"
        "#define TUNING_STEP_INTERVAL_MS UINT64_C(120)",
    )
    replace_once(
        radio_input,
        """static int left_stick_action(uint8_t y)
{
    if (y < STICK_LOW)
        return RADIO_INPUT_VOLUME_UP;
    if (y > STICK_HIGH)
        return RADIO_INPUT_VOLUME_DOWN;
    return -1;
}

static int right_stick_action(uint8_t x)
{
    if (x < STICK_LOW)
        return RADIO_INPUT_STATION_PREVIOUS;
    if (x > STICK_HIGH)
        return RADIO_INPUT_STATION_NEXT;
    return -1;
}
""",
        ROTARY_HELPERS,
    )
    replace_once(
        radio_input,
        """static int left_stick_key = -1;
static uint64_t left_stick_repeat_at;
static int right_stick_key = -1;
static uint64_t right_stick_repeat_at;
""",
        """static float left_stick_angle;
static float left_stick_accum;
static bool left_rotary_active;
static uint64_t left_step_at;
static float right_stick_angle;
static float right_stick_accum;
static bool right_rotary_active;
static uint64_t right_step_at;
""",
    )
    replace_once(
        radio_input,
        """    const uint64_t now = monotonic_milliseconds();
    const int volume_action = neutral ? -1 : left_stick_action(sample[5]);
    const int station_action = neutral ? -1 : right_stick_action(sample[6]);
    update_analog_action(volume_action, &left_stick_key, &left_stick_repeat_at, now,
                         STICK_REPEAT_DELAY_MS);
    update_analog_action(station_action, &right_stick_key, &right_stick_repeat_at, now,
                         TUNING_REPEAT_DELAY_MS);
""",
        """    const uint64_t now = monotonic_milliseconds();
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
""",
    )
    replace_once(
        radio_input,
        """    const uint64_t now = monotonic_milliseconds();
    repeat_analog_action(left_stick_key, &left_stick_repeat_at, now, STICK_REPEAT_MS);
    repeat_analog_action(right_stick_key, &right_stick_repeat_at, now, TUNING_REPEAT_MS);
""",
        "",
    )
    replace_once(
        radio_input,
        """    queue_read = queue_write = 0;
    button_state = 0;
    left_stick_key = -1;
    left_stick_repeat_at = 0;
    right_stick_key = -1;
    right_stick_repeat_at = 0;
    return true;
}""",
        """    queue_read = queue_write = 0;
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
}""",
    )
    replace_once(
        radio_input,
        """        owns_user_service = false;
    }
    queue_read = queue_write = 0;
    button_state = 0;
    left_stick_key = -1;
    left_stick_repeat_at = 0;
    right_stick_key = -1;
    right_stick_repeat_at = 0;
}""",
        """        owns_user_service = false;
    }
    queue_read = queue_write = 0;
    button_state = 0;
    left_rotary_active = false;
    right_rotary_active = false;
}""",
    )
    replace_once(
        radio_input,
        """static void queue_push(radio_input_key_t key, bool pressed)
{""",
        """static void queue_push(radio_input_key_t key, bool pressed);

""" + ROTARY_UPDATE + """
static void queue_push(radio_input_key_t key, bool pressed)
{""",
    )


def patch_radio_app_theme(src: Path, include: Path) -> None:
    header = include / "radio_app.hpp"
    replace_once(
        header,
        "    unsigned settings_focus_ = 0;",
        "    unsigned settings_focus_ = 0;\n"
        "    int theme_index_ = 0;\n"
        "    unsigned volume_frame_ = 20;\n"
        "    unsigned tuner_state_ = 0;",
    )
    replace_once(
        header,
        """    bool Initialize(Rml::ElementDocument* document);
    void Poll();
    void HandleInput(const radio_input_event_t& event);
    void Shutdown();

private:""",
        """    bool Initialize(Rml::ElementDocument* document);
    void Poll();
    void HandleInput(const radio_input_event_t& event);
    void Shutdown();

private:
    void LoadTheme();
    void SaveTheme() const;
    void ApplyTheme();
    void CycleTheme(int direction);
    void ApplyVolumeFrame();
    void ApplyTunerFrame();
""",
    )
    app = src / "radio_app.cpp"
    replace_once(
        app,
        """    RebuildFacets();
    RefreshAll();
    UpdateFocus();
    return true;
}""",
        """    RebuildFacets();
    LoadTheme();
    ApplyTheme();
    ApplyVolumeFrame();
    ApplyTunerFrame();
    RefreshAll();
    UpdateFocus();
    return true;
}""",
    )
    replace_once(
        app,
        """    const int next = std::clamp(current + direction * 2, 0, 100);
    if (next != current)
        radio_service_set_volume(static_cast<unsigned>(next));
    RefreshSettings(false);""",
        """    const int next = std::clamp(current + direction * 2, 0, 100);
    if (next != current)
    {
        radio_service_set_volume(static_cast<unsigned>(next));
        ApplyVolumeFrame();
    }
    RefreshSettings(false);""",
    )
    replace_once(
        app,
        """    focus_ = selected_slot_;
    BuildVisibleList();""",
        """    tuner_state_ = direction < 0 ? 1U : 2U;
    ApplyTunerFrame();
    focus_ = selected_slot_;
    BuildVisibleList();""",
    )
    replace_once(
        app,
        """    SetClass(document_, "settings-favorites", "focused", settings_focus_ == 0U);
    SetClass(document_, "settings-refresh", "focused", settings_focus_ == 1U);""",
        """    static const char *theme_names[] = {"Walnut", "Silver", "Graphite"};
    SetText(document_, "settings-theme-value",
            theme_names[theme_index_ % (sizeof(theme_names) / sizeof(theme_names[0]))]);
    SetClass(document_, "settings-theme-row", "focused", settings_focus_ == 0U);
    SetClass(document_, "settings-favorites", "focused", settings_focus_ == 1U);
    SetClass(document_, "settings-refresh", "focused", settings_focus_ == 2U);""",
    )
    replace_once(
        app,
        """void RadioApp::HandleSettingsKey(radio_input_key_t key)
{
    if (key == RADIO_INPUT_TRIANGLE || key == RADIO_INPUT_CIRCLE)
    {
        CloseSettings();
        return;
    }
    if (key == RADIO_INPUT_LEFT || key == RADIO_INPUT_RIGHT)
    {
        SetView(key == RADIO_INPUT_LEFT ? -1 : 1);
        RefreshSettings();
        return;
    }
    if (key == RADIO_INPUT_UP || key == RADIO_INPUT_DOWN)
    {
        settings_focus_ = settings_focus_ == 0U ? 1U : 0U;
        RefreshSettings();
        return;
    }
    if (key != RADIO_INPUT_CROSS)
        return;
    if (settings_focus_ == 0U)
    {
        CloseSettings();
        view_ = View::Favorites;
        page_start_ = selected_slot_ = focus_ = 0;
        RefreshAll();
        UpdateFocus();
        return;
    }
    radio_service_refresh();
    RefreshSettings();
}""",
        """void RadioApp::HandleSettingsKey(radio_input_key_t key)
{
    if (key == RADIO_INPUT_TRIANGLE || key == RADIO_INPUT_CIRCLE)
    {
        CloseSettings();
        return;
    }
    if (key == RADIO_INPUT_LEFT || key == RADIO_INPUT_RIGHT)
    {
        if (settings_focus_ == 0U)
            CycleTheme(key == RADIO_INPUT_LEFT ? -1 : 1);
        else
            SetView(key == RADIO_INPUT_LEFT ? -1 : 1);
        RefreshSettings();
        return;
    }
    if (key == RADIO_INPUT_UP || key == RADIO_INPUT_DOWN)
    {
        const int next = static_cast<int>(settings_focus_) + (key == RADIO_INPUT_DOWN ? 1 : -1);
        settings_focus_ = static_cast<unsigned>((next + 3) % 3);
        RefreshSettings();
        return;
    }
    if (key != RADIO_INPUT_CROSS)
        return;
    if (settings_focus_ == 0U)
    {
        CycleTheme(1);
        return;
    }
    if (settings_focus_ == 1U)
    {
        CloseSettings();
        view_ = View::Favorites;
        page_start_ = selected_slot_ = focus_ = 0;
        RefreshAll();
        UpdateFocus();
        return;
    }
    radio_service_refresh();
    RefreshSettings();
}

void RadioApp::LoadTheme()
{
    std::FILE *file = std::fopen("/download0/radio-theme.txt", "rb");
    if (file == nullptr)
        return;
    char buffer[8]{};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    if (read == 0)
        return;
    if (buffer[0] >= '0' && buffer[0] <= '2')
        theme_index_ = buffer[0] - '0';
}

void RadioApp::SaveTheme() const
{
    std::FILE *file = std::fopen("/download0/radio-theme.txt", "wb");
    if (file == nullptr)
        return;
    std::fprintf(file, "%d\\n", theme_index_);
    std::fclose(file);
}

void RadioApp::ApplyTheme()
{
    SetClass(document_, "radio-backdrop", "backdrop-hidden", theme_index_ != 0);
    SetClass(document_, "radio-backdrop-silver", "backdrop-hidden", theme_index_ != 1);
    SetClass(document_, "radio-backdrop-graphite", "backdrop-hidden", theme_index_ != 2);
    /* The control atlas is calibrated for the hybrid walnut front; the flat
     * finishes keep their own printed controls. */
    SetClass(document_, "controls-layer", "hidden", theme_index_ != 0);
}

void RadioApp::CycleTheme(int direction)
{
    theme_index_ = (theme_index_ + 3 + (direction > 0 ? 1 : -1)) % 3;
    ApplyTheme();
    SaveTheme();
}

void RadioApp::ApplyVolumeFrame()
{
    const unsigned volume = radio_service_get_volume();
    const unsigned frame = std::clamp((volume * 2U + 5U) / 10U, 0U, 20U);
    if (frame == volume_frame_)
        return;
    char previous_name[32];
    char current_name[32];
    std::snprintf(previous_name, sizeof(previous_name), "volume-frame-%u", volume_frame_);
    std::snprintf(current_name, sizeof(current_name), "volume-frame-%u", frame);
    SetClass(document_, "volume-frame", previous_name, false);
    SetClass(document_, "volume-frame", current_name, true);
    volume_frame_ = frame;
}

void RadioApp::ApplyTunerFrame()
{
    SetClass(document_, "tuner-frame", "tuner-idle", tuner_state_ == 0U);
    SetClass(document_, "tuner-frame", "tuner-prev", tuner_state_ == 1U);
    SetClass(document_, "tuner-frame", "tuner-next", tuner_state_ == 2U);
}""",
    )


def generate_controls_rcss(overlay: Path, worktree: Path) -> None:
    manifest = json.loads(
        (overlay / "assets/ui/controls/manifest-hybrid.json").read_text(encoding="utf-8")
    )
    lines = [
        "/* Generated at build time from controls/manifest-hybrid.json — do not edit. */",
        "",
    ]

    def sprite_block(name: str, file: str, frames: list[tuple[str, int, int, int, int]], cell: tuple[int, int], gutter: int) -> None:
        lines.append(f"@spritesheet {name} {{")
        lines.append(f"    src: ../controls/{file};")
        core_w, core_h = cell[0] - 2 * gutter, cell[1] - 2 * gutter
        for sprite, x, y, _w, _h in frames:
            lines.append(f"    {sprite}: {x + gutter}px {y + gutter}px {core_w}px {core_h}px;")
        lines.append("}")
        lines.append("")

    volume = manifest["volume"]
    vol_gutter = volume["extrudedGutterPx"]
    frames = [
        (f"volume_frame_{level['index']:02d}", level["x"], level["y"], level["width"], level["height"])
        for level in volume["levels"]
    ]
    focus = volume["focusOverlay"]
    frames.append(("volume_focus_ring", focus["x"], focus["y"], focus["width"], focus["height"]))
    sprite_block("volume-atlas", volume["file"], frames, volume["cell"], vol_gutter)

    tuner = manifest["tuner"]
    tuner_frames = [
        (sprite, frame["x"], frame["y"], frame["width"], frame["height"])
        for sprite, frame in tuner["frames"].items()
    ]
    sprite_block("tuner-atlas", tuner["file"], tuner_frames, tuner["cell"], tuner["extrudedGutterPx"])

    buttons = manifest["buttons"]
    button_frames = []
    for control, info in buttons["controls"].items():
        for state, frame in info["frames"].items():
            button_frames.append((f"btn_{control}_{state}", frame["x"], frame["y"], frame["width"], frame["height"]))
    sprite_block("buttons-atlas", buttons["file"], button_frames, buttons["cell"], buttons["extrudedGutterPx"])

    vx, vy, vw, vh = volume["logicalTargetRect"]
    lines.append(
        f"#volume-frame {{ position: absolute; left: {vx:.1f}px; top: {vy:.1f}px; "
        f"width: {vw:.1f}px; height: {vh:.1f}px; }}"
    )
    for index in range(21):
        lines.append(f'.volume-frame-{index} {{ decorator: sprite("volume_frame_{index:02d}"); }}')
    lines.append(
        f"#volume-focus-ring {{ position: absolute; left: {vx:.1f}px; top: {vy:.1f}px; "
        f"width: {vw:.1f}px; height: {vh:.1f}px; }}"
    )
    lines.append('.volume-focus-ring-on { decorator: sprite("volume_focus_ring"); }')
    lines.append("")

    tx, ty, tw, th = tuner["targetRect"]
    lines.append(
        f"#tuner-frame {{ position: absolute; left: {tx:.1f}px; top: {ty:.1f}px; "
        f"width: {tw:.1f}px; height: {th:.1f}px; }}"
    )
    lines.append('.tuner-idle { decorator: sprite("tuner_idle"); }')
    lines.append('.tuner-prev { decorator: sprite("tuner_previous_focus"); }')
    lines.append('.tuner-next { decorator: sprite("tuner_next_focus"); }')
    lines.append("")

    for control, info in buttons["controls"].items():
        x, y, w, h = info["targetRect"]
        element = f"btn-{control.replace('_', '-')}"
        lines.append(
            f"#{element} {{ position: absolute; left: {x:.1f}px; top: {y:.1f}px; "
            f"width: {w:.1f}px; height: {h:.1f}px; }}"
        )
        lines.append(f'.{element}-normal {{ decorator: sprite("btn_{control}_normal"); }}')
        lines.append(f'.{element}-selected {{ decorator: sprite("btn_{control}_selected"); }}')
    lines.append("")

    destination = worktree / "assets" / "ui" / "styles" / "controls.rcss"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(lines), encoding="utf-8")


def patch_console_ux(worktree: Path, overlay: Path) -> None:
    src = worktree / "src"
    # radio_app.cpp/.hpp ship complete from the overlay (theme, atlas frames and
    # the physical-button state machine are native there now).
    patch_volume_taper(src)
    patch_rotary_sticks(src)
    generate_controls_rcss(overlay, worktree)
