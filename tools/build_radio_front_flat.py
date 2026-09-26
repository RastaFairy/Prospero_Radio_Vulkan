#!/usr/bin/env python3
"""Build the orthographic Prospero Radio face and its runtime texture files.

The radio front is drawn from geometric primitives at 4K, so no camera
perspective, vanishing point, or elliptical knob can leak into the UI artwork.
The current KTX2 container metadata and complete mip chain layout are retained.
Requires Pillow; no external image converter is needed.
"""

from __future__ import annotations

import math
import random
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "overlay/assets/ui/art"
DOCS = ROOT / "docs"
MASTER = ART / "radio_front_master.png"
BASE = ART / "radio_front_base.png"
SOURCE_TGA = ART / "radio_front_4k.tga"
FALLBACK_TGA = ART / "radio_front_1080p.tga"
KTX2 = ART / "radio_front_4k.ktx2"
WIDTH, HEIGHT = 3840, 2160
LOGICAL_WIDTH, LOGICAL_HEIGHT = 1920, 1080
SCALE = 2


def px(value: float) -> int:
    return round(value * SCALE)


def box(rect: tuple[float, float, float, float]) -> tuple[int, int, int, int]:
    return tuple(px(value) for value in rect)


def blend(start: tuple[int, int, int], end: tuple[int, int, int], amount: float) -> tuple[int, int, int]:
    amount = max(0.0, min(1.0, amount))
    return tuple(round(a + (b - a) * amount) for a, b in zip(start, end))


def gradient_surface(
    width: int,
    height: int,
    top: tuple[int, int, int],
    bottom: tuple[int, int, int],
    seed: int,
    horizontal_brush: bool = False,
) -> Image.Image:
    image = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        color = blend(top, bottom, y / max(1, height - 1))
        draw.line((0, y, width, y), fill=color)
    if horizontal_brush:
        rng = random.Random(seed)
        for y in range(1, height, 3):
            offset = rng.choice((-5, -3, -2, -1, 1, 2, 3, 5))
            color = tuple(max(0, min(255, channel + offset)) for channel in blend(top, bottom, y / max(1, height - 1)))
            draw.line((0, y, width, y), fill=color, width=1)
    return image


def rounded_gradient(
    target: Image.Image,
    rect: tuple[float, float, float, float],
    top: tuple[int, int, int],
    bottom: tuple[int, int, int],
    radius: float,
    seed: int,
    brush: bool = False,
) -> None:
    x0, y0, x1, y1 = box(rect)
    surface = gradient_surface(x1 - x0, y1 - y0, top, bottom, seed, brush)
    mask = Image.new("L", surface.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, surface.width - 1, surface.height - 1), radius=px(radius), fill=255
    )
    target.paste(surface, (x0, y0), mask)


def rounded_mask(size: tuple[int, int], radius: int) -> Image.Image:
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size[0] - 1, size[1] - 1), radius=radius, fill=255
    )
    return mask


def make_wood_panel(width: int, height: int, seed: int) -> Image.Image:
    panel = gradient_surface(width, height, (76, 40, 25), (49, 26, 18), seed)
    draw = ImageDraw.Draw(panel)
    rng = random.Random(seed)
    for _ in range(120):
        x = rng.randrange(-12, width + 12)
        drift = rng.uniform(2.0, 13.0)
        phase = rng.uniform(0, math.tau)
        points = []
        for step in range(11):
            y = round(step * (height - 1) / 10)
            wave = math.sin(step * 0.63 + phase) * drift
            points.append((round(x + wave), y))
        base = rng.choice((35, 43, 53, 66, 79))
        color = (base + 13, base - 2, max(10, base - 13))
        draw.line(points, fill=color, width=rng.choice((1, 1, 2, 3)))
    for x in range(8, width, 17):
        shade = 36 + (x * 13 + seed) % 28
        draw.line((x, 0, x, height), fill=(shade + 14, shade, shade - 9), width=1)
    mask = rounded_mask(panel.size, px(8))
    panel.putalpha(mask)
    return panel


def font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = (
        [Path("C:/Windows/Fonts/seguisb.ttf"), Path("C:/Windows/Fonts/arialbd.ttf")]
        if bold
        else [Path("C:/Windows/Fonts/segoeui.ttf"), Path("C:/Windows/Fonts/arial.ttf")]
    )
    for candidate in candidates:
        if candidate.is_file():
            return ImageFont.truetype(str(candidate), px(size))
    return ImageFont.load_default()


def make_metal_knob(radius: int) -> Image.Image:
    diameter = px(radius * 2)
    image = Image.new("RGBA", (diameter, diameter), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    center = diameter / 2

    def circle(rad: float, fill: tuple[int, int, int], outline: tuple[int, int, int] | None = None, width: int = 1) -> None:
        r = px(rad)
        bounds = (round(center - r), round(center - r), round(center + r), round(center + r))
        draw.ellipse(bounds, fill=fill, outline=outline, width=px(width))

    # Perfectly concentric circular bezel and face; the rings are in the image
    # plane and are not foreshortened or tilted.
    circle(radius - 1, (5, 6, 7), (5, 6, 7), 1)
    circle(radius - 5, (45, 47, 49), (142, 139, 132), 2)
    circle(radius - 10, (21, 23, 25), (7, 8, 9), 2)
    face_radius = radius - 15
    face_size = px(face_radius * 2)
    face = gradient_surface(face_size, face_size, (178, 178, 174), (83, 85, 89), seed=radius, horizontal_brush=True)
    face_mask = Image.new("L", face.size, 0)
    ImageDraw.Draw(face_mask).ellipse((1, 1, face.width - 2, face.height - 2), fill=255)
    face.putalpha(face_mask)
    offset = px(15)
    image.alpha_composite(face, (offset, offset))
    draw = ImageDraw.Draw(image)

    center_bounds = (round(center - px(face_radius)), round(center - px(face_radius)),
                     round(center + px(face_radius)), round(center + px(face_radius)))
    draw.ellipse(center_bounds, outline=(203, 199, 189), width=px(1))
    inner = px(face_radius - 5)
    draw.ellipse((round(center - inner), round(center - inner), round(center + inner), round(center + inner)),
                 outline=(67, 69, 72), width=px(1))
    # A subtle central hub is exactly on the geometric center, like the user's
    # dial reference. Runtime volume indication is drawn by the atlas overlay.
    hub = px(3)
    draw.ellipse((round(center - hub), round(center - hub), round(center + hub), round(center + hub)),
                 fill=(83, 85, 88), outline=(195, 191, 181), width=px(1))
    return image


def point_on_circle(cx: float, cy: float, radius: float, angle: float) -> tuple[int, int]:
    radians = math.radians(angle)
    return px(cx + math.cos(radians) * radius), px(cy + math.sin(radians) * radius)


def draw_scale(draw: ImageDraw.ImageDraw, cx: float, cy: float) -> None:
    for index in range(21):
        angle = 135 + 270 * index / 20
        major = index % 4 == 0
        inner = 120 if major else 124
        outer = 137 if major else 133
        color = (143, 143, 137) if major else (91, 92, 91)
        draw.line((point_on_circle(cx, cy, inner, angle), point_on_circle(cx, cy, outer, angle)),
                  fill=color, width=px(1.5 if major else 1))


def draw_dial(canvas: Image.Image, cx: float, cy: float, ticks: bool) -> None:
    draw = ImageDraw.Draw(canvas)
    if ticks:
        draw_scale(draw, cx, cy)
        draw.ellipse(box((cx - 140, cy - 140, cx + 140, cy + 140)),
                     outline=(45, 47, 48), width=px(1))
    knob = make_metal_knob(104)
    canvas.alpha_composite(knob, (px(cx - 104), px(cy - 104)))


def draw_wood_cheek(canvas: Image.Image, rect: tuple[int, int, int, int], seed: int) -> None:
    left, top, right, bottom = rect
    panel = make_wood_panel(px(right - left), px(bottom - top), seed)
    canvas.alpha_composite(panel, (px(left), px(top)))
    draw = ImageDraw.Draw(canvas)
    edge_x = right - 3 if left < 960 else left + 3
    draw.line((px(edge_x), px(top + 12), px(edge_x), px(bottom - 12)),
              fill=(140, 89, 48, 220), width=px(1))


def draw_arrow(draw: ImageDraw.ImageDraw, center_x: int, center_y: int, previous: bool) -> None:
    color = (191, 190, 183)
    x, y = px(center_x), px(center_y)
    half_w, half_h = px(11), px(10)
    if previous:
        draw.line((x - half_w, y, x + half_w, y), fill=color, width=px(2))
        draw.line((x - half_w, y, x - half_w + px(9), y - half_h), fill=color, width=px(2))
        draw.line((x - half_w, y, x - half_w + px(9), y + half_h), fill=color, width=px(2))
        draw.line((x + half_w + px(5), y - half_h, x + half_w + px(5), y + half_h), fill=color, width=px(2))
    else:
        draw.line((x - half_w, y, x + half_w, y), fill=color, width=px(2))
        draw.line((x + half_w, y, x + half_w - px(9), y - half_h), fill=color, width=px(2))
        draw.line((x + half_w, y, x + half_w - px(9), y + half_h), fill=color, width=px(2))
        draw.line((x - half_w - px(5), y - half_h, x - half_w - px(5), y + half_h), fill=color, width=px(2))


def build_art() -> Image.Image:
    canvas = Image.new("RGBA", (WIDTH, HEIGHT), (13, 14, 16, 255))
    draw = ImageDraw.Draw(canvas)

    # A restrained, uniform studio backdrop in the radio's graphite/walnut
    # palette. No room geometry or perspective lines compete with the face.
    for y in range(HEIGHT):
        t = y / max(1, HEIGHT - 1)
        color = blend((15, 17, 19), (30, 20, 14), t)
        draw.line((0, y, WIDTH, y), fill=(*color, 255))
    glow = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse(box((160, 218, 1760, 922)), fill=(145, 81, 37, 55))
    glow = glow.filter(ImageFilter.GaussianBlur(px(175)))
    canvas = Image.alpha_composite(canvas, glow)

    # Soft contact shadow under a front-facing, perfectly horizontal chassis.
    shadow = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(box((74, 326, 1846, 802)), radius=px(22),
                                              fill=(0, 0, 0, 220))
    canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(px(22))))
    draw = ImageDraw.Draw(canvas)

    # Symmetric outer case and flat wooden end caps.
    draw.rounded_rectangle(box((86, 294, 1834, 786)), radius=px(18),
                           fill=(7, 8, 9), outline=(102, 73, 48), width=px(2))
    draw_wood_cheek(canvas, (96, 304, 184, 776), seed=81)
    draw_wood_cheek(canvas, (1736, 304, 1824, 776), seed=81)

    # One planar metal fascia, split into dark glass/controls and a satin lower
    # strip. Horizontal hairlines are brushed texture, never camera shading.
    rounded_gradient(canvas, (174, 306, 1746, 774), (48, 50, 51), (18, 20, 22), 12, 11, True)
    rounded_gradient(canvas, (174, 674, 1746, 774), (169, 168, 160), (82, 84, 86), 8, 12, True)
    draw = ImageDraw.Draw(canvas)
    draw.line((px(180), px(674), px(1740), px(674)), fill=(208, 205, 194), width=px(1))
    draw.line((px(182), px(676), px(1738), px(676)), fill=(47, 48, 49), width=px(1))
    draw.rounded_rectangle(box((174, 306, 1746, 774)), radius=px(12),
                           outline=(7, 8, 9), width=px(2))
    draw.line((px(188), px(317), px(1732), px(317)), fill=(112, 113, 110), width=px(1))

    # Product name and labels; centered baselines keep them on the same flat plane.
    text = ImageDraw.Draw(canvas)
    text.text((px(205), px(329)), "PROSPERO RADIO", font=font(20, True), fill=(220, 221, 217))
    text.text((px(365), px(366)), "VOLUME", font=font(12), anchor="mm", fill=(187, 188, 184))
    text.text((px(1555), px(366)), "TUNING", font=font(12), anchor="mm", fill=(187, 188, 184))

    # Center aperture: preserve the dark physical bezel and give its display a
    # matte, smoked amber glass look inspired by 1990s radio VFD/LCD panels.
    # Runtime station data is drawn over this material by the app.
    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle(box((507, 353, 1413, 645)), radius=16,
                           fill=(5, 6, 7), outline=(132, 128, 120), width=px(2))
    draw.rounded_rectangle(box((517, 363, 1403, 635)), radius=13,
                           fill=(25, 19, 15), outline=(48, 34, 22), width=px(2))
    lcd_rect = box((529, 375, 1391, 623))
    lcd = gradient_surface(
        lcd_rect[2] - lcd_rect[0], lcd_rect[3] - lcd_rect[1],
        (53, 36, 21), (35, 24, 15), seed=19,
    ).convert("RGBA")
    lcd_mask = rounded_mask(lcd.size, px(9))
    lcd.putalpha(lcd_mask)
    canvas.alpha_composite(lcd, (lcd_rect[0], lcd_rect[1]))
    # A very soft central amber bloom suggests an old phosphor display without
    # making the surface glossy or adding visible scanline bands.
    bloom = Image.new("RGBA", lcd.size, (0, 0, 0, 0))
    ImageDraw.Draw(bloom).ellipse(
        (px(105), px(18), lcd.width - px(105), lcd.height - px(18)),
        fill=(216, 112, 35, 21),
    )
    bloom = bloom.filter(ImageFilter.GaussianBlur(px(55)))
    bloom.putalpha(bloom.getchannel("A").point(lambda alpha: round(alpha * 0.45)))
    canvas.alpha_composite(bloom, (lcd_rect[0], lcd_rect[1]))

    # Dials share one exact horizontal axis and use circular, concentric bezels.
    # Volume marks are a clean 0..100 scale in 21 equal five-point steps.
    draw_dial(canvas, 365, 520, ticks=True)
    draw_dial(canvas, 1555, 520, ticks=False)

    draw = ImageDraw.Draw(canvas)
    draw.text((px(264), px(666)), "−", font=font(16), fill=(162, 163, 159), anchor="mm")
    draw.text((px(466), px(666)), "+", font=font(16), fill=(162, 163, 159), anchor="mm")

    # The right control is a digital previous/next switch beneath the knob.
    draw_arrow(draw, 1510, 645, previous=True)
    draw_arrow(draw, 1600, 645, previous=False)

    # Headphone socket is circular and lower-left; its caption is etched into
    # the satin strip. All seven keys are equal-height and form a centered row.
    draw.ellipse(box((205, 716, 237, 748)), fill=(6, 7, 8), outline=(194, 191, 183), width=px(2))
    draw.ellipse(box((214, 725, 228, 739)), fill=(2, 3, 4), outline=(53, 55, 57), width=px(1))
    text.text((px(221), px(756)), "HEADPHONES", font=font(8), anchor="mm", fill=(44, 45, 45))

    labels = ("HOME", "RADIO", "FAVORITES", "GENRES", "SEARCH", "SETTINGS", "PLAY")
    key_start, key_width, gap = 434, 146, 5
    for index, label in enumerate(labels):
        x0 = key_start + index * (key_width + gap)
        x1 = x0 + key_width
        y0, y1 = 700, 758
        draw.rounded_rectangle(box((x0, y0, x1, y1)), radius=5,
                               fill=(6, 7, 8), outline=(178, 175, 168), width=px(1))
        draw.rounded_rectangle(box((x0 + 3, y0 + 3, x1 - 3, y1 - 3)), radius=4,
                               fill=(20, 22, 23), outline=(52, 54, 55), width=px(1))
        if index == 0:
            cx, cy = (x0 + x1) // 2, 726
            draw.polygon([(px(cx - 8), px(cy)), (px(cx), px(cy - 7)), (px(cx + 8), px(cy))],
                         fill=(207, 207, 202))
            draw.rectangle(box((cx - 6, cy, cx + 6, cy + 9)), fill=(207, 207, 202))
        elif index == 6:
            cx, cy = (x0 + x1) // 2, 728
            draw.polygon([(px(cx - 15), px(cy - 9)), (px(cx - 15), px(cy + 9)), (px(cx - 1), px(cy))],
                         fill=(207, 207, 202))
            draw.line((px(cx + 5), px(cy - 9), px(cx + 5), px(cy + 9)), fill=(207, 207, 202), width=px(3))
            draw.line((px(cx + 13), px(cy - 9), px(cx + 13), px(cy + 9)), fill=(207, 207, 202), width=px(3))
        else:
            label_font = font(10 if label in ("FAVORITES", "SETTINGS") else 11, True)
            text.text((px((x0 + x1) / 2), px(729)), label, font=label_font,
                      anchor="mm", fill=(201, 202, 198))

    # Tiny panel separators and fasteners are balanced left/right.
    draw = ImageDraw.Draw(canvas)
    for x in (184, 1736):
        draw.ellipse(box((x - 2, 326, x + 2, 330)), fill=(103, 104, 102))
        draw.ellipse(box((x - 2, 655, x + 2, 659)), fill=(91, 92, 89))

    return canvas


def tga_bytes(image: Image.Image) -> bytes:
    image = image.convert("RGBA")
    width, height = image.size
    header = bytearray(18)
    header[2] = 2
    struct.pack_into("<HHHHBB", header, 8, 0, 0, width, height, 32, 0x28)
    return bytes(header) + image.tobytes("raw", "BGRA")


def rebuild_ktx2(image: Image.Image) -> None:
    if not KTX2.is_file():
        raise SystemExit(f"Missing current KTX2 container metadata: {KTX2}")
    old = bytearray(KTX2.read_bytes())
    identifier = bytes((0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A))
    if old[:12] != identifier:
        raise SystemExit(f"Invalid KTX2 container: {KTX2}")
    fields = struct.unpack_from("<9I", old, 12)
    width, height, mip_levels = fields[2], fields[3], fields[7]
    if (width, height) != image.size:
        raise SystemExit(f"KTX2 is {width}x{height}; art is {image.width}x{image.height}")
    if mip_levels != max(width, height).bit_length():
        raise SystemExit(f"KTX2 has {mip_levels} levels; expected a complete mip chain")

    current = image.convert("RGBA")
    mip_payloads: list[tuple[int, bytes]] = []
    for level in range(mip_levels):
        index_offset = 80 + level * 24
        payload_offset, byte_length, uncompressed_length = struct.unpack_from("<3Q", old, index_offset)
        payload = current.tobytes("raw", "RGBA")
        if len(payload) != byte_length or len(payload) != uncompressed_length:
            raise SystemExit(f"Unexpected mip payload size at level {level}")
        mip_payloads.append((payload_offset, payload))
        if level + 1 < mip_levels:
            size = (max(1, current.width // 2), max(1, current.height // 2))
            current = current.resize(size, Image.Resampling.LANCZOS)
    # Patch only the existing mip payload ranges. Opening with wb would ask
    # Windows to truncate/replace this tracked asset, which can be rejected by
    # the active file-sharing policy even when in-place writes are allowed.
    with KTX2.open("r+b") as stream:
        for offset, payload in mip_payloads:
            stream.seek(offset)
            written = stream.write(payload)
            if written != len(payload):
                raise OSError(f"Short KTX2 mip write at level offset {offset}")
        stream.flush()


def main() -> None:
    ART.mkdir(parents=True, exist_ok=True)
    DOCS.mkdir(parents=True, exist_ok=True)
    art = build_art()
    if art.size != (WIDTH, HEIGHT):
        raise SystemExit(f"Unexpected artwork size: {art.size}")

    master = art.resize((LOGICAL_WIDTH, LOGICAL_HEIGHT), Image.Resampling.LANCZOS).convert("RGB")
    master.save(MASTER, optimize=True)
    master.save(BASE, optimize=True)
    SOURCE_TGA.write_bytes(tga_bytes(art))
    FALLBACK_TGA.write_bytes(tga_bytes(master.convert("RGBA")))
    rebuild_ktx2(art)

    preview_path = DOCS / "radio-front-flat-preview.png"
    master.save(preview_path, optimize=True)
    print(f"Created centered orthographic radio front: {MASTER}")
    print(f"Runtime source: {SOURCE_TGA} ({SOURCE_TGA.stat().st_size:,} bytes)")
    print(f"Runtime KTX2: {KTX2} ({KTX2.stat().st_size:,} bytes)")
    print(f"Preview: {preview_path}")


if __name__ == "__main__":
    main()
