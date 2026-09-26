#!/usr/bin/env python3
"""Build runtime-ready Prospero Radio control atlases from the 4K backplate.

Requires Pillow for authoring only. The app consumes the generated 32-bit TGA
atlases; PNG previews and the JSON map are development references.
"""

from __future__ import annotations

import json
import math
import struct
from copy import deepcopy
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "overlay/assets/ui/art/radio_front_4k.tga"
OUT = ROOT / "overlay/assets/ui/controls"
DOCS = ROOT / "docs"

SOURCE_REFERENCE_SIZE = (1920, 1080)
BACKPLATE_SIZE = (3840, 2160)
LOGICAL_SIZE = (1920, 1080)

AMBER = (226, 166, 88, 255)
AMBER_BRIGHT = (244, 190, 118, 255)
AMBER_DIM = (188, 126, 57, 170)
LCD_PRIMARY = (244, 190, 118, 255)
LCD_SECONDARY = (226, 166, 88, 255)
LCD_MUTED = (188, 126, 57, 255)
INK = (12, 13, 14, 255)

BUTTON_CELL = (320, 128)
BUTTONS = [
    ("home", "Inicio", 434, 580),
    ("radio", "Radio", 585, 731),
    ("favorites", "Favoritos", 736, 882),
    ("genres", "Generos", 887, 1033),
    ("search", "Buscar", 1038, 1184),
    ("settings", "Ajustes", 1189, 1335),
    ("play_pause", "Reproducir / pausa", 1340, 1486),
]
BUTTON_STATES = ["normal", "focus", "pressed", "selected", "selected_focus"]
BUTTON_Y = (700, 758)

VOLUME_CELL = 400
VOLUME_COLUMNS = 4
VOLUME_ROWS = 6
VOLUME_LEVELS = list(range(0, 101, 5))
VOLUME_CENTER_REFERENCE = (365.0, 520.0)
VOLUME_TARGET_SIZE = 288
VOLUME_FOCUS_FRAME = len(VOLUME_LEVELS)

# Exact 2x scale of the 166x48 logical target keeps the two printed arrows
# geometrically aligned when their white strokes are recolored by an overlay.
TUNER_CELL = (332, 96)
TUNER_FRAMES = [
    "idle",
    "previous_focus",
    "previous_pressed",
    "next_focus",
    "next_pressed",
]
TUNER_LOGICAL_RECT = (1472.0, 621.0, 166.0, 48.0)

ART_FRAME_SIZE = 320
ATLAS_GUTTER = 8

HYBRID_SOURCE_RECT = (86.0, 294.0, 1748.0, 492.0)
HYBRID_TARGET_WIDTH = 1850.0
HYBRID_SCALE = HYBRID_TARGET_WIDTH / HYBRID_SOURCE_RECT[2]
HYBRID_TARGET_RECT = (
    (1920.0 - HYBRID_TARGET_WIDTH) / 2,
    (1080.0 - HYBRID_SOURCE_RECT[3] * HYBRID_SCALE) / 2,
    HYBRID_TARGET_WIDTH,
    HYBRID_SOURCE_RECT[3] * HYBRID_SCALE,
)


def rgba_tga_bytes(image: Image.Image) -> bytes:
    """Encode top-left-origin, uncompressed 32-bit TGA accepted by the PS5 loader."""
    image = image.convert("RGBA")
    width, height = image.size
    if width > 65535 or height > 65535:
        raise ValueError("TGA dimensions exceed the 16-bit format limit")
    header = bytearray(18)
    header[2] = 2
    struct.pack_into("<HHHHBB", header, 8, 0, 0, width, height, 32, 0x28)
    return bytes(header) + image.tobytes("raw", "BGRA")


def save_runtime_tga(path: Path, image: Image.Image) -> None:
    data = rgba_tga_bytes(image)
    if len(data) >= 16 * 1024 * 1024:
        raise ValueError(f"{path.name} exceeds the renderer's 16 MiB whole-file limit")
    path.write_bytes(data)


def scale_x(value: float) -> float:
    return value * BACKPLATE_SIZE[0] / SOURCE_REFERENCE_SIZE[0]


def scale_y(value: float) -> float:
    return value * BACKPLATE_SIZE[1] / SOURCE_REFERENCE_SIZE[1]


def to_logical_rect(x: float, y: float, width: float, height: float) -> list[float]:
    return [
        round(x * LOGICAL_SIZE[0] / SOURCE_REFERENCE_SIZE[0], 2),
        round(y * LOGICAL_SIZE[1] / SOURCE_REFERENCE_SIZE[1], 2),
        round(width * LOGICAL_SIZE[0] / SOURCE_REFERENCE_SIZE[0], 2),
        round(height * LOGICAL_SIZE[1] / SOURCE_REFERENCE_SIZE[1], 2),
    ]


def atlas_frame_xy(frame_size: tuple[int, int], columns: int, index: int) -> tuple[int, int]:
    return (
        (index % columns) * (frame_size[0] + ATLAS_GUTTER * 2) + ATLAS_GUTTER,
        (index // columns) * (frame_size[1] + ATLAS_GUTTER * 2) + ATLAS_GUTTER,
    )


def paste_with_extruded_gutter(atlas: Image.Image, frame: Image.Image, x: int, y: int) -> None:
    """Place the frame plus an edge-extruded border around its UV core."""
    gutter = ATLAS_GUTTER
    width, height = frame.size
    atlas.alpha_composite(frame, (x, y))

    top = frame.crop((0, 0, width, 1)).resize((width, gutter), Image.Resampling.NEAREST)
    bottom = frame.crop((0, height - 1, width, height)).resize((width, gutter), Image.Resampling.NEAREST)
    left = frame.crop((0, 0, 1, height)).resize((gutter, height), Image.Resampling.NEAREST)
    right = frame.crop((width - 1, 0, width, height)).resize((gutter, height), Image.Resampling.NEAREST)
    atlas.alpha_composite(top, (x, y - gutter))
    atlas.alpha_composite(bottom, (x, y + height))
    atlas.alpha_composite(left, (x - gutter, y))
    atlas.alpha_composite(right, (x + width, y))

    corners = (
        (0, 0, x - gutter, y - gutter),
        (width - 1, 0, x + width, y - gutter),
        (0, height - 1, x - gutter, y + height),
        (width - 1, height - 1, x + width, y + height),
    )
    for sx, sy, dx, dy in corners:
        corner = Image.new("RGBA", (gutter, gutter), frame.getpixel((sx, sy)))
        atlas.alpha_composite(corner, (dx, dy))


def hybrid_point(x: float, y: float) -> list[float]:
    source_x, source_y, _, _ = HYBRID_SOURCE_RECT
    target_x, target_y, _, _ = HYBRID_TARGET_RECT
    return [
        round(target_x + (x - source_x) * HYBRID_SCALE, 2),
        round(target_y + (y - source_y) * HYBRID_SCALE, 2),
    ]


def hybrid_rect(rect: list[float] | tuple[float, ...]) -> list[float]:
    x, y, width, height = rect
    left, top = hybrid_point(x, y)
    right, bottom = hybrid_point(x + width, y + height)
    return [left, top, round(right - left, 2), round(bottom - top, 2)]


def hybrid_manifest(flat: dict) -> dict:
    """Map the flat-face manifest onto the scaled fascia in the warm backplate."""
    result = deepcopy(flat)
    result["schema"] = "prospero-radio-controls-hybrid/v1"
    result["source"] = "../art/radio_front_hybrid_4k.ktx2"
    result["composition"]["radioBounds"] = [round(v, 2) for v in HYBRID_TARGET_RECT]
    result["composition"]["displayRect"] = hybrid_rect(result["composition"]["displayRect"])
    result["composition"]["displayContentRect"] = hybrid_rect(result["composition"]["displayContentRect"])
    result["composition"]["leftDialCenter"] = hybrid_point(*result["composition"]["leftDialCenter"])
    result["composition"]["rightDialCenter"] = hybrid_point(*result["composition"]["rightDialCenter"])
    result["composition"]["rearChassisVisible"] = True

    for control in result["buttons"]["controls"].values():
        control["targetRect"] = hybrid_rect(control["targetRect"])
    result["volume"]["logicalTargetRect"] = hybrid_rect(result["volume"]["logicalTargetRect"])
    result["volume"]["logicalCenter"] = hybrid_point(*result["volume"]["logicalCenter"])
    result["volume"]["drawOrder"][0] = "radio_front_hybrid_4k.ktx2"
    result["tuner"]["targetRect"] = hybrid_rect(result["tuner"]["targetRect"])
    for direction in result["tuner"]["directions"].values():
        direction["logicalCenter"] = hybrid_point(*direction["logicalCenter"])
    result["screen"]["rect"] = hybrid_rect(result["screen"]["rect"])
    result["screen"]["contentRect"] = hybrid_rect(result["screen"]["contentRect"])
    result["runtimeTexture"]["sampling"] = (
        "KTX2 backplate uses mipmapping; TGA atlases use single-level nearest/clamp, "
        "with 8px edge-extruded gutters for filter safety"
    )
    result["runtimeTexture"]["atlasGutterPx"] = ATLAS_GUTTER
    result["runtimeTexture"]["atlasMipmaps"] = False
    result["runtimeTexture"]["atlasUvCoreExcludesGutter"] = True
    return result


def rounded_mask(size: tuple[int, int], radius: int) -> Image.Image:
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return mask


def tint(frame: Image.Image, color: tuple[int, int, int, int]) -> Image.Image:
    return Image.alpha_composite(frame, Image.new("RGBA", frame.size, color))


def add_button_focus(frame: Image.Image, active: bool = False) -> Image.Image:
    glow = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.rounded_rectangle((3, 3, frame.width - 4, frame.height - 4), 10,
                         outline=(221, 153, 76, 65), width=9)
    glow = glow.filter(ImageFilter.GaussianBlur(4))
    frame = Image.alpha_composite(frame, glow)
    draw = ImageDraw.Draw(frame)
    draw.rounded_rectangle((5, 5, frame.width - 6, frame.height - 6), 8,
                           outline=AMBER_BRIGHT, width=3)
    if active:
        add_button_selected(frame)
    return frame


def add_button_selected(frame: Image.Image) -> None:
    draw = ImageDraw.Draw(frame)
    draw.rounded_rectangle((82, frame.height - 12, frame.width - 82, frame.height - 8),
                           2, fill=AMBER)


def make_button_frame(source: Image.Image, bounds: tuple[int, int, int, int], state: str) -> Image.Image:
    x0, x1 = bounds[0], bounds[1]
    y0, y1 = BUTTON_Y
    sx = BACKPLATE_SIZE[0] / SOURCE_REFERENCE_SIZE[0]
    sy = BACKPLATE_SIZE[1] / SOURCE_REFERENCE_SIZE[1]
    crop = source.crop((round(x0 * sx), round(y0 * sy), round(x1 * sx), round(y1 * sy)))
    frame = crop.resize(BUTTON_CELL, Image.Resampling.LANCZOS).convert("RGBA")
    mask = rounded_mask(BUTTON_CELL, 7)
    frame.putalpha(mask)

    if state in ("focus", "selected_focus"):
        if state == "selected_focus":
            frame = tint(frame, (213, 146, 72, 12))
        frame = add_button_focus(frame, active=(state == "selected_focus"))
    elif state == "pressed":
        frame = tint(frame, (0, 0, 0, 50))
        draw = ImageDraw.Draw(frame)
        draw.rounded_rectangle((5, 5, frame.width - 6, frame.height - 6), 8,
                               outline=(205, 139, 69, 210), width=2)
        draw.line((18, 9, frame.width - 18, 9), fill=(231, 179, 109, 110), width=2)
    elif state == "selected":
        add_button_selected(frame)

    frame.putalpha(mask)
    return frame


def make_button_atlas(source: Image.Image) -> tuple[Image.Image, dict]:
    cell_w, cell_h = BUTTON_CELL
    columns, rows = len(BUTTONS), len(BUTTON_STATES)
    atlas = Image.new("RGBA", (columns * (cell_w + 2 * ATLAS_GUTTER),
                                rows * (cell_h + 2 * ATLAS_GUTTER)), (0, 0, 0, 0))
    entries = {}
    for row, state in enumerate(BUTTON_STATES):
        for column, (key, label, x0, x1) in enumerate(BUTTONS):
            frame = make_button_frame(source, (x0, x1, *BUTTON_Y), state)
            frame_x, frame_y = atlas_frame_xy(BUTTON_CELL, columns, row * columns + column)
            paste_with_extruded_gutter(atlas, frame, frame_x, frame_y)
            entries.setdefault(key, {"labelEs": label, "targetRect": to_logical_rect(x0, BUTTON_Y[0], x1 - x0, BUTTON_Y[1] - BUTTON_Y[0]), "frames": {}})
            entries[key]["frames"][state] = {
                "index": row * len(BUTTONS) + column,
                "x": frame_x,
                "y": frame_y,
                "width": cell_w,
                "height": cell_h,
                "uv": [
                    round(frame_x / atlas.width, 6),
                    round(frame_y / atlas.height, 6),
                    round((frame_x + cell_w) / atlas.width, 6),
                    round((frame_y + cell_h) / atlas.height, 6),
                ],
            }
    return atlas, {
        "file": "buttons.tga",
        "dimensions": list(atlas.size),
        "cell": list(BUTTON_CELL),
        "columns": columns,
        "rows": rows,
        "extrudedGutterPx": ATLAS_GUTTER,
        "mipLevels": 1,
        "stateRows": BUTTON_STATES,
        "controls": entries,
    }


def dial_point(radius: float, angle_degrees: float) -> tuple[float, float]:
    angle = math.radians(angle_degrees)
    return (VOLUME_CELL / 2 + math.cos(angle) * radius,
            VOLUME_CELL / 2 + math.sin(angle) * radius)


def make_volume_frame(level_index: int) -> Image.Image:
    # The static front texture owns the brushed-metal dial. These animation
    # frames contain only the changing amber scale, arc and indicator, so a
    # frame can never replace the knob face with a cropped opaque disc.
    frame = Image.new("RGBA", (VOLUME_CELL, VOLUME_CELL), (0, 0, 0, 0))
    draw = ImageDraw.Draw(frame)
    center = (VOLUME_CELL / 2, VOLUME_CELL / 2)
    value_angle = 135.0 + 270.0 * level_index / 20.0

    # The base art contains quiet reference marks. This frame illuminates the
    # active marks and adds the corresponding amber arc and pointer.
    if level_index > 0:
        draw.arc((46, 46, VOLUME_CELL - 47, VOLUME_CELL - 47), 135,
                 value_angle, fill=AMBER_DIM, width=4)
    for index in range(21):
        angle = 135.0 + 270.0 * index / 20.0
        major = index % 4 == 0
        inner_radius = 167 if major else 172
        outer_radius = 190 if major else 185
        color = AMBER if index <= level_index else (44, 41, 37, 180)
        draw.line((dial_point(inner_radius, angle), dial_point(outer_radius, angle)),
                  fill=color, width=4 if major else 3)

    pointer_start = dial_point(10, value_angle)
    pointer_end = dial_point(108, value_angle)
    draw.line((pointer_start, pointer_end), fill=AMBER_BRIGHT, width=4)
    px, py = dial_point(117, value_angle)
    draw.ellipse((px - 7, py - 7, px + 7, py + 7), fill=(237, 177, 98, 100))
    draw.ellipse((px - 4, py - 4, px + 4, py + 4), fill=AMBER_BRIGHT)
    draw.ellipse((center[0] - 4, center[1] - 4, center[0] + 4, center[1] + 4),
                 fill=(191, 128, 61, 220))

    return frame


def make_volume_focus_frame() -> Image.Image:
    frame = Image.new("RGBA", (VOLUME_CELL, VOLUME_CELL), (0, 0, 0, 0))
    draw = ImageDraw.Draw(frame)
    draw.ellipse((6, 6, VOLUME_CELL - 7, VOLUME_CELL - 7),
                 outline=(228, 169, 96, 105), width=3)
    for angle in (135, 202.5, 270, 337.5, 45):
        x0, y0 = dial_point(181, angle)
        x1, y1 = dial_point(196, angle)
        draw.line((x0, y0, x1, y1), fill=AMBER_BRIGHT, width=3)
    return frame


def make_volume_atlas() -> tuple[Image.Image, dict]:
    atlas = Image.new("RGBA", (VOLUME_COLUMNS * (VOLUME_CELL + 2 * ATLAS_GUTTER),
                                VOLUME_ROWS * (VOLUME_CELL + 2 * ATLAS_GUTTER)), (0, 0, 0, 0))
    frames = []
    for index, value in enumerate(VOLUME_LEVELS):
        frame = make_volume_frame(index)
        x, y = atlas_frame_xy((VOLUME_CELL, VOLUME_CELL), VOLUME_COLUMNS, index)
        paste_with_extruded_gutter(atlas, frame, x, y)
        frames.append({
            "value": value,
            "index": index,
            "x": x,
            "y": y,
            "width": VOLUME_CELL,
            "height": VOLUME_CELL,
            "uv": [round(x / atlas.width, 6), round(y / atlas.height, 6),
                   round((x + VOLUME_CELL) / atlas.width, 6),
                   round((y + VOLUME_CELL) / atlas.height, 6)],
        })
    focus_x, focus_y = atlas_frame_xy((VOLUME_CELL, VOLUME_CELL), VOLUME_COLUMNS, VOLUME_FOCUS_FRAME)
    paste_with_extruded_gutter(atlas, make_volume_focus_frame(), focus_x, focus_y)
    return atlas, {
        "file": "volume.tga",
        "dimensions": list(atlas.size),
        "cell": [VOLUME_CELL, VOLUME_CELL],
        "columns": VOLUME_COLUMNS,
        "rows": VOLUME_ROWS,
        "extrudedGutterPx": ATLAS_GUTTER,
        "mipLevels": 1,
        "content": "transparent overlay; static metallic dial remains in radio_front_4k.ktx2",
        "levels": frames,
        "focusOverlay": {
            "index": VOLUME_FOCUS_FRAME,
            "x": focus_x,
            "y": focus_y,
            "width": VOLUME_CELL,
            "height": VOLUME_CELL,
            "uv": [round(focus_x / atlas.width, 6), round(focus_y / atlas.height, 6),
                   round((focus_x + VOLUME_CELL) / atlas.width, 6),
                   round((focus_y + VOLUME_CELL) / atlas.height, 6)],
            "drawOverCurrentLevel": True,
        },
        "unusedCells": [22, 23],
    }


def make_tuner_frame(state: str) -> Image.Image:
    frame = Image.new("RGBA", TUNER_CELL, (0, 0, 0, 0))
    if state == "idle":
        return frame
    direction = "previous" if state.startswith("previous") else "next"
    center_x = 76 if direction == "previous" else 256
    center_y = TUNER_CELL[1] // 2
    pressed = state.endswith("pressed")
    draw = ImageDraw.Draw(frame)
    # The base backplate already contains the printed ivory arrow. Overlay the
    # same strokes in amber only; a surrounding box would clash with the face.
    color = AMBER_BRIGHT if pressed else AMBER
    draw_tuner_arrow(draw, center_x, center_y, direction == "previous", color)
    return frame


def draw_tuner_arrow(draw: ImageDraw.ImageDraw, center_x: float, center_y: float,
                     previous: bool, color: tuple[int, int, int, int],
                     scale: float = 1.0) -> None:
    half_w, half_h = 22 * scale, 20 * scale
    stroke = max(1, round(4 * scale))
    offset = 10 * scale
    x, y = center_x, center_y
    if previous:
        draw.line((x - half_w, y, x + half_w, y), fill=color, width=stroke)
        draw.line((x - half_w, y, x - half_w + 18 * scale, y - half_h), fill=color, width=stroke)
        draw.line((x - half_w, y, x - half_w + 18 * scale, y + half_h), fill=color, width=stroke)
        draw.line((x + half_w + offset, y - half_h, x + half_w + offset, y + half_h),
                  fill=color, width=stroke)
    else:
        draw.line((x - half_w, y, x + half_w, y), fill=color, width=stroke)
        draw.line((x + half_w, y, x + half_w - 18 * scale, y - half_h), fill=color, width=stroke)
        draw.line((x + half_w, y, x + half_w - 18 * scale, y + half_h), fill=color, width=stroke)
        draw.line((x - half_w - offset, y - half_h, x - half_w - offset, y + half_h),
                  fill=color, width=stroke)


def make_tuner_atlas() -> tuple[Image.Image, dict]:
    atlas = Image.new("RGBA", (len(TUNER_FRAMES) * (TUNER_CELL[0] + 2 * ATLAS_GUTTER),
                                TUNER_CELL[1] + 2 * ATLAS_GUTTER), (0, 0, 0, 0))
    entries = {}
    for index, state in enumerate(TUNER_FRAMES):
        frame = make_tuner_frame(state)
        frame_x, frame_y = atlas_frame_xy(TUNER_CELL, len(TUNER_FRAMES), index)
        paste_with_extruded_gutter(atlas, frame, frame_x, frame_y)
        entries[state] = {
            "index": index,
            "x": frame_x,
            "y": frame_y,
            "width": TUNER_CELL[0],
            "height": TUNER_CELL[1],
            "uv": [round(frame_x / atlas.width, 6), round(frame_y / atlas.height, 6),
                   round((frame_x + TUNER_CELL[0]) / atlas.width, 6),
                   round((frame_y + TUNER_CELL[1]) / atlas.height, 6)],
        }
    return atlas, {
        "file": "tuner.tga",
        "dimensions": list(atlas.size),
        "cell": list(TUNER_CELL),
        "extrudedGutterPx": ATLAS_GUTTER,
        "mipLevels": 1,
        "frames": entries,
        "targetRect": list(TUNER_LOGICAL_RECT),
        "directions": {
            "previous": {"logicalCenter": [1510, 645], "action": "previous_station"},
            "next": {"logicalCenter": [1600, 645], "action": "next_station"},
        },
        "semantics": "Digital directional previous/next buttons; never rotate the tuner knob.",
    }


def make_album_frame() -> Image.Image:
    frame = Image.new("RGBA", (ART_FRAME_SIZE, ART_FRAME_SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(frame)
    draw.rounded_rectangle((3, 3, ART_FRAME_SIZE - 4, ART_FRAME_SIZE - 4), 9,
                           outline=(155, 154, 149, 220), width=3)
    draw.rounded_rectangle((9, 9, ART_FRAME_SIZE - 10, ART_FRAME_SIZE - 10), 6,
                           outline=(49, 48, 45, 210), width=2)
    return frame


def load_font(size: int) -> ImageFont.ImageFont:
    candidates = [
        Path("C:/Windows/Fonts/arial.ttf"),
        Path("C:/Windows/Fonts/segoeui.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def make_composite_preview(source: Image.Image, button_atlas: Image.Image,
                           volume_atlas: Image.Image, tuner_atlas: Image.Image) -> Image.Image:
    preview = source.resize(SOURCE_REFERENCE_SIZE, Image.Resampling.LANCZOS).convert("RGBA")

    # Show volume at 65%, focused Favorites, and the tuner "next" direction in focus.
    level_index = 13
    vx, vy = atlas_frame_xy((VOLUME_CELL, VOLUME_CELL), VOLUME_COLUMNS, level_index)
    dial = volume_atlas.crop((vx, vy, vx + VOLUME_CELL, vy + VOLUME_CELL))
    ref_size = VOLUME_TARGET_SIZE
    dial = dial.resize((ref_size, ref_size), Image.Resampling.LANCZOS)
    cx, cy = VOLUME_CENTER_REFERENCE
    preview.alpha_composite(dial, (round(cx - ref_size / 2), round(cy - ref_size / 2)))

    button_index = 2
    state_row = BUTTON_STATES.index("selected_focus")
    bx, by = atlas_frame_xy(BUTTON_CELL, len(BUTTONS), state_row * len(BUTTONS) + button_index)
    button = button_atlas.crop((bx, by, bx + BUTTON_CELL[0], by + BUTTON_CELL[1]))
    x0, x1 = BUTTONS[button_index][2:]
    bw = round(x1 - x0)
    bh = BUTTON_Y[1] - BUTTON_Y[0]
    button = button.resize((bw, bh), Image.Resampling.LANCZOS)
    preview.alpha_composite(button, (x0, BUTTON_Y[0]))

    tuner_index = TUNER_FRAMES.index("next_focus")
    tx, ty = atlas_frame_xy(TUNER_CELL, len(TUNER_FRAMES), tuner_index)
    tuner = tuner_atlas.crop((tx, ty, tx + TUNER_CELL[0], ty + TUNER_CELL[1]))
    logical_x, logical_y, logical_w, logical_h = TUNER_LOGICAL_RECT
    target = (
        round(logical_x * SOURCE_REFERENCE_SIZE[0] / LOGICAL_SIZE[0]),
        round(logical_y * SOURCE_REFERENCE_SIZE[1] / LOGICAL_SIZE[1]),
        round(logical_w * SOURCE_REFERENCE_SIZE[0] / LOGICAL_SIZE[0]),
        round(logical_h * SOURCE_REFERENCE_SIZE[1] / LOGICAL_SIZE[1]),
    )
    tuner = tuner.resize((target[2], target[3]), Image.Resampling.LANCZOS)
    preview.alpha_composite(tuner, (target[0], target[1]))

    # Illustrative station metadata remains live UI text, not baked into either atlas.
    draw = ImageDraw.Draw(preview)
    title_font = load_font(36)
    body_font = load_font(21)
    small_font = load_font(18)
    left, top = 704, 413
    draw.text((left, top), "MANGORADIO", fill=LCD_PRIMARY, font=title_font)
    draw.text((left, top + 48), "Germany / Rheinland-Pfalz  |  English, German",
              fill=LCD_SECONDARY, font=body_font)
    draw.text((left, top + 81), "music  |  MP3 128 kbps",
              fill=LCD_MUTED, font=body_font)
    draw.line((left, top + 116, 1228, top + 116), fill=(116, 74, 36, 230), width=1)
    draw.text((left, top + 128), "NOW PLAYING", fill=LCD_PRIMARY, font=small_font)
    draw.text((left, top + 158), "Track title and artist when supplied by the stream",
              fill=LCD_SECONDARY, font=small_font)
    return preview.convert("RGB")


def make_atlas_contact_sheet(button_atlas: Image.Image, volume_atlas: Image.Image,
                             tuner_atlas: Image.Image, source: Image.Image) -> Image.Image:
    width, height = 1920, 1480
    canvas = Image.new("RGB", (width, height), (24, 26, 27))
    draw = ImageDraw.Draw(canvas)
    title = load_font(34)
    section = load_font(24)
    caption = load_font(17)
    draw.text((42, 28), "PROSPERO RADIO · ATLAS DE CONTROLES", fill=(230, 232, 231), font=title)

    # Volume: 21 level poses + the separate focus overlay, matching the runtime atlas.
    draw.text((42, 93), "VOLUMEN · 0–100 en pasos de 5", fill=(223, 173, 105), font=section)
    cell = 174
    sx = BACKPLATE_SIZE[0] / SOURCE_REFERENCE_SIZE[0]
    sy = BACKPLATE_SIZE[1] / SOURCE_REFERENCE_SIZE[1]
    center_x = VOLUME_CENTER_REFERENCE[0] * sx
    center_y = VOLUME_CENTER_REFERENCE[1] * sy
    base_half = VOLUME_TARGET_SIZE * sx / 2
    base_dial = source.crop((round(center_x - base_half), round(center_y - base_half),
                             round(center_x + base_half), round(center_y + base_half)))
    base_dial = base_dial.resize((cell, cell), Image.Resampling.LANCZOS).convert("RGBA")
    for index, value in enumerate(VOLUME_LEVELS):
        col = index % VOLUME_COLUMNS
        x = 44 + col * 185
        y = 135 + (index // VOLUME_COLUMNS) * 205
        frame_x, frame_y = atlas_frame_xy((VOLUME_CELL, VOLUME_CELL), VOLUME_COLUMNS, index)
        frame = volume_atlas.crop((frame_x, frame_y,
                                   frame_x + VOLUME_CELL, frame_y + VOLUME_CELL))
        frame = frame.resize((cell, cell), Image.Resampling.LANCZOS)
        dial = base_dial.copy()
        dial.alpha_composite(frame)
        canvas.paste(dial.convert("RGB"), (x, y))
        draw.rectangle((x, y, x + cell - 1, y + cell - 1), outline=(92, 84, 72), width=1)
        draw.text((x + 8, y + 7), f"{value}%", fill=(239, 226, 208), font=caption)
    focus_index = VOLUME_FOCUS_FRAME
    col, row = focus_index % VOLUME_COLUMNS, focus_index // VOLUME_COLUMNS
    x = 44 + col * 185
    y = 135 + row * 205
    frame_x, frame_y = atlas_frame_xy((VOLUME_CELL, VOLUME_CELL), VOLUME_COLUMNS, focus_index)
    frame = volume_atlas.crop((frame_x, frame_y,
                               frame_x + VOLUME_CELL, frame_y + VOLUME_CELL))
    frame = frame.resize((cell, cell), Image.Resampling.LANCZOS)
    dial = base_dial.copy()
    dial.alpha_composite(frame)
    canvas.paste(dial.convert("RGB"), (x, y))
    draw.rectangle((x, y, x + cell - 1, y + cell - 1), outline=(223, 173, 105), width=2)
    draw.text((x + 8, y + 7), "FOCO", fill=(239, 199, 135), font=caption)

    # Buttons: controls across columns, states down rows.
    right_x = 810
    draw.text((right_x, 93), "BOTONES FÍSICOS", fill=(223, 173, 105), font=section)
    mini_w, mini_h = 120, 48
    gap_x, gap_y = 6, 22
    for row, state in enumerate(BUTTON_STATES):
        row_y = 134 + row * (mini_h + gap_y)
        draw.text((right_x, row_y + 17), state.replace("_", " ").upper(),
                  fill=(175, 178, 178), font=caption)
        for col, button in enumerate(BUTTONS):
            x = right_x + 158 + col * (mini_w + gap_x)
            source_x, source_y = atlas_frame_xy(BUTTON_CELL, len(BUTTONS), row * len(BUTTONS) + col)
            frame = button_atlas.crop((source_x, source_y,
                                       source_x + BUTTON_CELL[0], source_y + BUTTON_CELL[1]))
            frame = frame.resize((mini_w, mini_h), Image.Resampling.LANCZOS)
            canvas.paste(frame, (x, row_y), frame)

    # Directional tuner: this is a discrete switch, not a rotating dial.
    tuner_y = 600
    draw.text((right_x, tuner_y), "TUNER DIGITAL · ANTERIOR / SIGUIENTE", fill=(223, 173, 105), font=section)
    for index, name in enumerate(TUNER_FRAMES):
        source_x, source_y = atlas_frame_xy(TUNER_CELL, len(TUNER_FRAMES), index)
        frame = tuner_atlas.crop((source_x, source_y,
                                  source_x + TUNER_CELL[0], source_y + TUNER_CELL[1]))
        frame = frame.resize((256, 74), Image.Resampling.LANCZOS)
        x = right_x + (index % 2) * 300
        y = tuner_y + 45 + (index // 2) * 105
        # Show the unchanged ivory print underneath the transparent amber tint.
        base = Image.new("RGBA", (256, 74), (39, 39, 38, 255))
        icon_draw = ImageDraw.Draw(base)
        preview_scale = 256 / TUNER_CELL[0]
        draw_tuner_arrow(icon_draw, 76 * preview_scale, 48 * preview_scale, True,
                         (191, 190, 183, 255), preview_scale)
        draw_tuner_arrow(icon_draw, 256 * preview_scale, 48 * preview_scale, False,
                         (191, 190, 183, 255), preview_scale)
        base.alpha_composite(frame)
        canvas.paste(base.convert("RGB"), (x, y))
        draw.text((x, y + 76), name.replace("_", " "), fill=(200, 203, 203), font=caption)

    draw.text((42, 1378), "TGA BGRA8 · UV solo al núcleo · padding extruido de 8 px · coordenadas en manifest*.json.",
              fill=(153, 157, 157), font=caption)
    return canvas


def main() -> None:
    if not SOURCE.is_file():
        raise SystemExit(f"Missing source backplate: {SOURCE}")
    OUT.mkdir(parents=True, exist_ok=True)
    DOCS.mkdir(parents=True, exist_ok=True)
    source = Image.open(SOURCE).convert("RGBA")
    if source.size != BACKPLATE_SIZE:
        raise SystemExit(f"Expected {BACKPLATE_SIZE} source image, got {source.size}")

    buttons, button_manifest = make_button_atlas(source)
    volume, volume_manifest = make_volume_atlas()
    tuner, tuner_manifest = make_tuner_atlas()
    cover = make_album_frame()

    save_runtime_tga(OUT / "buttons.tga", buttons)
    save_runtime_tga(OUT / "volume.tga", volume)
    save_runtime_tga(OUT / "tuner.tga", tuner)
    save_runtime_tga(OUT / "album-art-frame.tga", cover)

    manifest = {
        "schema": "prospero-radio-controls-atlas/v1",
        "source": "../art/radio_front_4k.ktx2",
        "coordinateSpace": {"width": 1920, "height": 1080, "origin": "top-left"},
        "composition": {
            "projection": "orthographic-front",
            "vanishingPoint": None,
            "radioBounds": [86, 294, 1748, 492],
            "displayRect": [507, 353, 906, 292],
            "displayContentRect": [540, 392, 840, 216],
            "displayFill": "matte smoked brown-amber 1990s radio display glass with soft center phosphor warmth; draw live metadata over it",
            "leftDialCenter": [365, 520],
            "rightDialCenter": [1555, 520],
            "dialGeometry": "true circles with concentric planar rings",
            "volumeStep": 5,
            "preserveAspectRatio": True,
        },
        "runtimeTexture": {
            "format": "TGA type 2, 32-bit BGRA, 8-bit alpha, top-left origin",
            "loader": "Ps5VulkanRenderInterface::LoadTexture",
            "sampling": "single-level TGA atlas; current nearest/clamp sampler",
            "atlasFramesUseHalfOpenUvRect": True,
            "atlasGutterPx": ATLAS_GUTTER,
            "atlasGutterMode": "edge-extruded",
            "atlasMipmaps": False,
            "atlasUvCoreExcludesGutter": True,
        },
        "buttons": button_manifest,
        "volume": {
            **volume_manifest,
            "step": 5,
            "rounding": "nearest 5; clamp to 0..100",
            "frameFormula": "clamp(floor((volume + 2.5) / 5), 0, 20)",
            "logicalTargetRect": to_logical_rect(
                VOLUME_CENTER_REFERENCE[0] - VOLUME_TARGET_SIZE / 2,
                VOLUME_CENTER_REFERENCE[1] - VOLUME_TARGET_SIZE / 2,
                VOLUME_TARGET_SIZE,
                VOLUME_TARGET_SIZE,
            ),
            "logicalCenter": [
                round(VOLUME_CENTER_REFERENCE[0] * LOGICAL_SIZE[0] / SOURCE_REFERENCE_SIZE[0], 2),
                round(VOLUME_CENTER_REFERENCE[1] * LOGICAL_SIZE[1] / SOURCE_REFERENCE_SIZE[1], 2),
            ],
            "drawOrder": ["radio_front_4k.ktx2", "volume-level-frame", "focus-overlay-when-volume-has-focus"],
        },
        "tuner": tuner_manifest,
        "albumArtwork": {
            "frameFile": "album-art-frame.tga",
            "frameDimensions": list(cover.size),
            "visibility": "Show only when actual station artwork is available; otherwise let text use the full display width.",
            "artworkIsDynamic": True,
        },
        "screen": {
            "rect": [507, 353, 906, 292],
            "contentRect": [540, 392, 840, 216],
            "backgroundTextIsBakedIn": False,
            "backgroundIsUniform": False,
            "surfaceStyle": "matte smoked amber display glass, 1990s radio VFD/LCD look, orange phosphor-style text",
            "recommendedTextColors": {
                "primary": "#F4BE76",
                "secondary": "#E2A658",
                "muted": "#BC7E39",
            },
            "contentSource": "live Radio Browser station metadata and stream now-playing metadata when supplied",
            "fields": ["station", "location_or_channel", "language", "codec", "bitrate", "artist", "track_title", "optional_album_art"],
        },
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    hybrid = hybrid_manifest(manifest)
    (OUT / "manifest-hybrid.json").write_text(
        json.dumps(hybrid, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    composite = make_composite_preview(source, buttons, volume, tuner)
    composite.save(DOCS / "radio-controls-composite-preview.png", optimize=True)
    sheet = make_atlas_contact_sheet(buttons, volume, tuner, source)
    sheet.save(DOCS / "radio-controls-atlas-preview.png", optimize=True)

    print(f"Generated control atlases in {OUT}")
    for path in sorted(OUT.glob("*.tga")):
        print(f"  {path.name}: {path.stat().st_size:,} bytes")
    print(f"Generated previews in {DOCS}")


if __name__ == "__main__":
    main()
