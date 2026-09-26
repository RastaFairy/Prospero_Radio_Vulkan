# Console UX integration patch for ProsperoRadio Vulkan Edition.
# Applied by overlay/apply-vulkan.py on the materialized upstream worktree:
#   - perceptual (audio-taper) volume curve for sceAudioOut,
#   - rotary potentiometer emulation on both analog sticks,
#   - cabinet-finish theme system with persistence in /download0,
#   - control-atlas wiring (volume frame overlay, tuner highlights)
#     using the spritesheet classes generated from manifest-hybrid.json.
from __future__ import annotations

import json
import re
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


def split_atlas(overlay: Path, worktree: Path) -> dict:
    """Split the control atlases into per-frame 32-bit TGAs (pure python, no
    Pillow): the RmlUi sprite decorators never rendered on the console, so the
    runtime uses stacked <img> elements with visibility toggles instead."""
    manifest = json.loads(
        (overlay / "assets/ui/controls/manifest-hybrid.json").read_text(encoding="utf-8")
    )
    destination = worktree / "assets" / "ui" / "controls" / "frames"
    destination.mkdir(parents=True, exist_ok=True)

    def read_tga(path: Path):
        data = path.read_bytes()
        width = data[12] | (data[13] << 8)
        height = data[14] | (data[15] << 8)
        bpp = data[16]
        top_down = (data[17] & 0x30) == 0x20
        pixels = data[18 + data[0]:]
        return width, height, bpp, top_down, pixels

    def crop(src, rect):
        width, height, bpp, top_down, pixels = src
        stride = bpp // 8
        x, y, w, h = rect
        row_bytes = w * stride
        out = bytearray(row_bytes * h)
        for row in range(h):
            source_row = y + row if top_down else y + h - 1 - row
            offset = ((source_row * width) + x) * stride
            out[row * row_bytes:(row + 1) * row_bytes] = pixels[offset:offset + row_bytes]
        return w, h, out

    def write_tga(path: Path, width, height, pixels):
        header = bytearray(18)
        header[2] = 2
        header[12] = width & 0xFF
        header[13] = (width >> 8) & 0xFF
        header[14] = height & 0xFF
        header[15] = (height >> 8) & 0xFF
        header[16] = 32
        header[17] = 0x28
        path.write_bytes(bytes(header) + bytes(pixels))

    frames = {}

    def emit(source_path: Path, rect, name: str, gutter: int):
        src = read_tga(source_path)
        x, y, w, h = rect
        core = (x + gutter, y + gutter, w - 2 * gutter, h - 2 * gutter)
        width, height, pixels = crop(src, core)
        file_name = f"{name}.tga"
        write_tga(destination / file_name, width, height, pixels)
        frames[name] = {"file": f"controls/frames/{file_name}", "rect": [float(x) for x in rect]}

    volume = manifest["volume"]
    vg = volume["extrudedGutterPx"]
    for level in volume["levels"]:
        emit(overlay / "assets/ui/controls" / volume["file"],
             (level["x"], level["y"], level["width"], level["height"]),
             f"volume_frame_{level['index']:02d}", vg)
    focus = volume["focusOverlay"]
    emit(overlay / "assets/ui/controls" / volume["file"],
         (focus["x"], focus["y"], focus["width"], focus["height"]), "volume_focus_ring", vg)

    tuner = manifest["tuner"]
    tg = tuner["extrudedGutterPx"]
    for state, frame in tuner["frames"].items():
        emit(overlay / "assets/ui/controls" / tuner["file"],
             (frame["x"], frame["y"], frame["width"], frame["height"]),
             f"tuner_{state}", tg)

    buttons = manifest["buttons"]
    bg = buttons["extrudedGutterPx"]
    for control, info in buttons["controls"].items():
        for state, frame in info["frames"].items():
            emit(overlay / "assets/ui/controls" / buttons["file"],
                 (frame["x"], frame["y"], frame["width"], frame["height"]),
                 f"btn_{control}_{state}", bg)
    return frames


def generate_controls_assets(overlay: Path, worktree: Path, version: str) -> None:
    NL = chr(10)
    frames = split_atlas(overlay, worktree)
    manifest = json.loads(
        (overlay / "assets/ui/controls/manifest-hybrid.json").read_text(encoding="utf-8")
    )

    def rect_css(rect):
        x, y, w, h = rect
        return f"left: {x:.1f}px; top: {y:.1f}px; width: {w:.1f}px; height: {h:.1f}px;"

    css = ["/* Generated at build time from controls/manifest-hybrid.json. */", ""]
    volume_rect = rect_css(manifest["volume"]["logicalTargetRect"])
    css.append(f".volume-frame-img {{ position: absolute; {volume_rect} }}")
    tuner_rect = rect_css(manifest["tuner"]["targetRect"])
    css.append(f".tuner-frame-img {{ position: absolute; {tuner_rect} }}")
    for control, info in manifest["buttons"]["controls"].items():
        css.append(
            f".btn-rect-{control.replace('_', '-')} {{ position: absolute; {rect_css(info['targetRect'])} }}"
        )
    css.append("")

    rml = ['<div id="controls-layer">']
    for index in range(21):
        visible = "" if index == 20 else " hidden"
        rml.append(
            f'    <img id="volume_frame_{index:02d}" class="volume-frame-img frame-img{visible}" '
            f'src="controls/frames/volume_frame_{index:02d}.tga" />')
    rml.append('    <img id="volume_focus_ring" class="volume-frame-img frame-img hidden" '
               'src="controls/frames/volume_focus_ring.tga" />')
    tuner_states = ["idle", "previous_focus", "previous_pressed", "next_focus", "next_pressed"]
    for state in tuner_states:
        visible = "" if state == "idle" else " hidden"
        rml.append(
            f'    <img id="tuner_{state}" class="tuner-frame-img frame-img{visible}" '
            f'src="controls/frames/tuner_{state}.tga" />')
    for control in manifest["buttons"]["controls"]:
        element = f"btn-{control.replace('_', '-')}"
        for state in manifest["buttons"]["stateRows"]:
            initial = " selected" if (control == "radio" and state == "selected") else (
                " hidden" if state != "normal" else "")
            rml.append(
                f'    <img id="{element}-{state}" class="btn-img btn-rect-{control.replace("_", "-")} frame-img{initial}" '
                f'src="controls/frames/btn_{control}_{state}.tga" />')
    rml.append("</div>")
    stack = chr(10).join(rml)

    rml_path = worktree / "assets" / "ui" / "main.rml"
    rml_text = rml_path.read_text(encoding="utf-8")
    # Replace the WHOLE controls-layer container: cutting at the first closing
    # tag after the last child leaves the container's own closer behind and
    # unbalances the document (app-shell closed early, parse errors).
    start = rml_text.index('<div id="controls-layer">')
    end = rml_text.index('<div id="display">', start)
    rml_text = rml_text[:start] + stack + NL + NL + rml_text[end:]

    # Validate: every <div> must be closed before </body>, no orphan closers.
    depth = 0
    for token in re.finditer(r"<div\b|</div>", rml_text):
        depth += 1 if token.group(0).startswith("<div") else -1
        if depth < 0:
            raise RuntimeError("generated main.rml closes more divs than it opens")
    if depth != 0:
        raise RuntimeError(f"generated main.rml leaves {depth} div(s) open")
    rml_text = rml_text.replace("__PROSPERO_VERSION__", version)
    rml_path.write_text(rml_text, encoding="utf-8")

    css_path = worktree / "assets" / "ui" / "styles" / "controls.rcss"
    css_path.write_text(chr(10).join(css), encoding="utf-8")



def patch_font_nearest_size(src: Path) -> None:
    """The bitmap engine registers faces only at the shipped sizes (20..48 for
    Montserrat); CSS sizes like 14/15/17/18 px found no face and flooded the
    runtime log with per-frame warnings while rendering no text. Match the
    nearest registered size of the same family/style/weight instead."""
    engine = src / "bitmap_font_engine.cpp"
    text = engine.read_text(encoding="utf-8")
    old = """    const Rml::String normalized_family = Rml::StringUtilities::ToLower(family);
    for (const auto &font : fonts)
    {
        if (font->Family() == normalized_family && font->Style() == style &&
            font->Weight() == weight && font->Metrics().size == size)
            return font.get();
    }
    return nullptr;
}"""
    new = """    const Rml::String normalized_family = Rml::StringUtilities::ToLower(family);
    for (const auto &font : fonts)
    {
        if (font->Family() == normalized_family && font->Style() == style &&
            font->Weight() == weight && font->Metrics().size == size)
            return font.get();
    }
    /* Nearest registered size: the interface uses sizes (14, 15, 17, 18 px)
     * that have no exact bitmap face; render with the closest one instead of
     * dropping the text. */
    BitmapFontFace *nearest = nullptr;
    int best_distance = 0;
    for (const auto &font : fonts)
    {
        if (font->Family() == normalized_family && font->Style() == style &&
            font->Weight() == weight)
        {
            const int distance = font->Metrics().size - size;
            const int magnitude = distance < 0 ? -distance : distance;
            if (nearest == nullptr || magnitude < best_distance)
            {
                nearest = font.get();
                best_distance = magnitude;
            }
        }
    }
    return nearest;
}"""
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Expected exactly one FindBitmapFont body, got {count}")
    engine.write_text(text.replace(old, new, 1), encoding="utf-8")

def patch_console_ux(worktree: Path, overlay: Path, version: str) -> None:
    src = worktree / "src"
    # radio_app.cpp/.hpp ship complete from the overlay (theme, atlas frames and
    # the physical-button state machine are native there now).
    patch_volume_taper(src)
    patch_rotary_sticks(src)
    patch_font_nearest_size(src)
    generate_controls_assets(overlay, worktree, version)
