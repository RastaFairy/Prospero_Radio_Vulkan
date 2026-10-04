"""Build the optional stacked CD/radio face and its amber disc icons."""

from __future__ import annotations

import json
import math
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageStat


ROOT = Path(__file__).resolve().parents[2]
GUIDE = ROOT / "out/cdda/prospero_radio_cd_stack_4k.png"
RADIO_SOURCE = ROOT / "overlay/assets/ui/art/radio_front_hybrid_4k.tga"
ART = ROOT / "overlay/assets/ui/art"
CONTROLS = ROOT / "overlay/assets/ui/controls"
BUTTON_MANIFEST = CONTROLS / "manifest-hybrid.json"
BUTTON_ATLAS = CONTROLS / "buttons.tga"
PREVIEW = ROOT / "out/cdda/prospero-radio-cd-stack-preview-v050.png"

LOGICAL_SIZE = (1920, 1080)
OUTPUT_SCALE = 2048 / 1920
OUTPUT_SIZE = (2048, 1152)
STACK_OVERLAY = ART / "prospero_cd_stack_overlay_2048.tga"
DISC_FRAME_COUNT = 12
DISC_FRAME_PREFIX = "cd_disc_frame_"

# Measured from the supplied 4K stack guide and converted to the 1920x1080
# RmlUi surface. The lower radio keeps its original size and moves down 130px.
LOWER_RADIO = (35, 280, 1885, 800)
LOWER_RADIO_SHIFT_Y = 130
CD_DECK = (35, 58, 1885, 408)

LEFT_SCREEN = (358, 113, 1041, 273)
RIGHT_SCREEN = (1160, 113, 1575, 273)
def px(value: float) -> int:
    return round(value * OUTPUT_SCALE)


def px_rect(rect: tuple[float, float, float, float]) -> tuple[int, int, int, int]:
    return tuple(px(value) for value in rect)


def write_tga(path: Path, image: Image.Image) -> None:
    image = image.convert("RGBA")
    width, height = image.size
    if not (0 < width <= 65535 and 0 < height <= 65535):
        raise ValueError(f"invalid TGA dimensions: {width}x{height}")
    header = bytearray(18)
    header[2] = 2
    struct.pack_into("<HHHHBB", header, 8, 0, 0, width, height, 32, 0x28)
    path.write_bytes(bytes(header) + image.tobytes("raw", "BGRA"))


def draw_brushed(size: tuple[int, int], sample: Image.Image,
                 bright: float, dark: float) -> Image.Image:
    width, height = size
    average = ImageStat.Stat(sample.convert("RGB")).mean
    output = Image.new("RGBA", size)
    draw = ImageDraw.Draw(output)
    for y in range(height):
        amount = y / max(1, height - 1)
        band = ((y * 17) % 13 - 6) * 0.34
        color = tuple(
            max(0, min(255, round(average[channel] +
                                  bright * (1.0 - amount) -
                                  dark * amount + band)))
            for channel in range(3)
        )
        draw.line((0, y, width, y), fill=(*color, 255))
    return output


def draw_screen(canvas: Image.Image,
                rect: tuple[int, int, int, int], amber: bool) -> None:
    draw = ImageDraw.Draw(canvas)
    x0, y0, x1, y1 = px_rect(rect)
    draw.rounded_rectangle((x0, y0, x1, y1), radius=px(9),
                           fill=(8, 8, 9, 255),
                           outline=(139, 135, 126, 255), width=px(3))
    inset = px(9)
    inner = (x0 + inset, y0 + inset, x1 - inset, y1 - inset)
    draw.rounded_rectangle(inner, radius=px(7),
                           fill=(52, 34, 19, 255) if amber else (5, 6, 7, 255),
                           outline=(67, 54, 40, 255) if amber else (43, 43, 42, 255),
                           width=px(2))
    if amber:
        glow = Image.new("RGBA", (inner[2] - inner[0], inner[3] - inner[1]))
        glow_draw = ImageDraw.Draw(glow)
        for y in range(glow.height):
            amount = y / max(1, glow.height - 1)
            color = (round(54 - 12 * amount), round(36 - 8 * amount),
                     round(20 - 5 * amount), 255)
            glow_draw.line((0, y, glow.width, y), fill=color)
        mask = Image.new("L", glow.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (0, 0, glow.width - 1, glow.height - 1), radius=px(6), fill=255
        )
        glow.putalpha(mask)
        canvas.alpha_composite(glow, inner[:2])


def add_receiver_overlay(source: Image.Image, overlay: Image.Image) -> None:
    x0, y0, x1, y1 = px_rect(LOWER_RADIO)
    receiver = source.crop((x0, y0, x1, y1)).convert("RGBA")
    mask = Image.new("L", receiver.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, receiver.width - 1, receiver.height - 1),
        radius=px(18), fill=255
    )
    receiver.putalpha(mask)
    overlay.alpha_composite(receiver, (x0, y0 + px(LOWER_RADIO_SHIFT_Y)))


def add_cd_deck(source: Image.Image, overlay: Image.Image) -> None:
    x0, y0, x1, y1 = px_rect(CD_DECK)
    draw = ImageDraw.Draw(overlay)
    radius = px(16)
    draw.rounded_rectangle((x0, y0, x1, y1), radius=radius,
                           fill=(7, 7, 8, 255), outline=(109, 78, 51, 255),
                           width=px(3))
    draw.rounded_rectangle((x0 + px(12), y0 + px(11), x1 - px(12), y1 - px(11)),
                           radius=px(12), fill=(35, 34, 32, 255),
                           outline=(95, 91, 84, 255), width=px(2))

    # Reuse the radio side-grain at native scale. Resizing the entire side
    # panel also compressed its trim and grain, which made the stacked seams
    # visibly disagree with the canonical lower receiver.
    wood_y0, wood_y1 = px(72), px(394)
    wood_source_y0 = px(350)
    wood_height = wood_y1 - wood_y0
    left_wood = source.crop((px(47), wood_source_y0,
                             px(132), wood_source_y0 + wood_height))
    right_wood = source.crop((px(1788), wood_source_y0,
                              px(1873), wood_source_y0 + wood_height))
    overlay.alpha_composite(left_wood.convert("RGBA"), (px(47), wood_y0))
    overlay.alpha_composite(right_wood.convert("RGBA"), (px(1788), wood_y0))

    # Reuse the radio's graphite and brushed-metal palette, with a clean upper
    # face so the CD list and transport controls have their own live areas.
    face_rect = (132, 75, 1788, 307)
    fx0, fy0, fx1, fy1 = px_rect(face_rect)
    face_sample = source.crop((px(160), px(299), px(1755), px(319)))
    face = draw_brushed((fx1 - fx0, fy1 - fy0), face_sample, 2.2, 7.0)
    overlay.alpha_composite(face, (fx0, fy0))

    metal_rect = (132, 307, 1788, 394)
    mx0, my0, mx1, my1 = px_rect(metal_rect)
    metal_sample = source.crop((px(300), px(687), px(402), px(781)))
    metal = draw_brushed((mx1 - mx0, my1 - my0), metal_sample, 34.0, 35.0)
    overlay.alpha_composite(metal, (mx0, my0))
    draw = ImageDraw.Draw(overlay)
    draw.line((mx0, my0, mx1, my0), fill=(222, 219, 211, 255), width=px(2))
    draw.line((mx0, my1 - px(2), mx1, my1 - px(2)),
              fill=(69, 68, 65, 255), width=px(2))

    draw_screen(overlay, LEFT_SCREEN, amber=False)
    draw_screen(overlay, RIGHT_SCREEN, amber=True)

def make_disc_frame(frame: int, frames: int = 12, size: int = 128) -> Image.Image:
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pixels = bytearray(size * size * 4)
    center = (size - 1) / 2.0
    phase = 2.0 * math.pi * frame / frames
    for y in range(size):
        for x in range(size):
            dx, dy = x - center, y - center
            radius = math.hypot(dx, dy)
            if radius > 60.0:
                continue
            angle = math.atan2(dy, dx)
            radial = radius / 60.0
            brushed = 5.0 * math.sin(radius * 0.66)
            shade = 1.0 - radial
            color = [round(base + shade * lift + brushed)
                     for base, lift in ((78, 19), (46, 14), (25, 8))]
            arc = (max(0.0, math.cos(angle - phase)) ** 18 *
                   max(0.0, 1.0 - abs(radius - 42.0) / 19.0))
            sheen = round(arc * 39.0)
            color = [min(208, color[0] + sheen),
                     min(145, color[1] + round(sheen * 0.69)),
                     min(74, color[2] + round(sheen * 0.31))]
            if any(abs(radius - band) < 0.8 for band in (20.0, 38.0, 55.5)):
                color = [min(limit, channel + lift)
                         for channel, limit, lift in zip(color, (208, 145, 74), (17, 12, 6))]
            if 5.2 <= radius <= 6.8 or 57.0 <= radius <= 59.2:
                color = [min(limit, channel + lift)
                         for channel, limit, lift in zip(color, (208, 145, 74), (27, 18, 9))]
            if 17.0 <= radius <= 18.2:
                color = (58, 37, 21)
            elif radius < 5.2:
                color = (37, 26, 17)
            offset = (y * size + x) * 4
            pixels[offset:offset + 4] = bytes((*color, 255))
    image.frombytes(bytes(pixels))
    draw = ImageDraw.Draw(image)
    draw.ellipse((center - 60, center - 60, center + 60, center + 60),
                 outline=(171, 112, 53, 255), width=1)
    draw.ellipse((center - 18, center - 18, center + 18, center + 18),
                 outline=(87, 56, 31, 255), width=1)
    draw.ellipse((center - 6, center - 6, center + 6, center + 6),
                 fill=(25, 18, 12, 255), outline=(161, 104, 47, 255), width=2)
    return image


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    path = Path("C:/Windows/Fonts") / name
    if not path.is_file():
        path = Path("C:/Windows/Fonts/arial.ttf")
    return ImageFont.truetype(str(path), px(size))


def draw_preview_buttons(preview: Image.Image) -> None:
    manifest = json.loads(BUTTON_MANIFEST.read_text(encoding="utf-8"))
    atlas = Image.open(BUTTON_ATLAS).convert("RGBA")
    gutter = manifest["buttons"]["extrudedGutterPx"]
    for control, info in manifest["buttons"]["controls"].items():
        # Preview the current focus on RADIO; all other keys are idle. Use the
        # exact runtime atlas frames and hybrid-manifest target rectangles.
        state = "focus" if control == "radio" else "normal"
        frame = info["frames"][state]
        x, y, width, height = (int(frame[key]) for key in ("x", "y", "width", "height"))
        button = atlas.crop((x + gutter, y + gutter,
                             x + width - gutter, y + height - gutter))
        target_x, target_y, target_width, target_height = info["targetRect"]
        button = button.resize((px(target_width), px(target_height)), Image.Resampling.LANCZOS)
        preview.alpha_composite(button, (px(target_x), px(target_y + LOWER_RADIO_SHIFT_Y)))


def draw_preview_content(preview: Image.Image, disc: Image.Image) -> None:
    draw = ImageDraw.Draw(preview)

    def text(x: float, y: float, value: str, size: int,
             color: tuple[int, int, int], bold: bool = False) -> None:
        draw.text((px(x), px(y)), value, font=font(size, bold), fill=(*color, 255))

    text(370, 120, "PROSPERO CD", 14, (240, 230, 214))
    text(1173, 120, "12 TRACKS  ·  DIAL SELECTS  ·  ↓ RADIO", 12, (189, 132, 63))
    for index in range(5):
        top = 145 + index * 23
        if index == 0:
            draw.rounded_rectangle((px(368), px(top - 1), px(1042), px(top + 20)),
                                   radius=px(3), fill=(83, 53, 28, 255))
        color = (255, 225, 173) if index == 0 else (207, 157, 92)
        if index == 0:
            text(372, top, "▶", 12, (255, 209, 138))
        text(396, top, f"CD AUDIO TRACK {index + 1:02d}", 15, color)
        text(912, top + 1, f"CD-DA  {index + 1:02d}/12", 12, (165, 118, 63))

    text(1173, 145, "DISC READY", 13, (214, 151, 75))
    text(1173, 171, "CD AUDIO TRACK 01", 18, (244, 190, 118), True)
    text(1173, 206, "TRACK 01 / 12  ·  CD-DA 44.1 kHz", 13, (190, 135, 69))
    disc_frame = disc.crop((0, 0, 128, 128)).resize((px(72), px(72)), Image.Resampling.LANCZOS)
    preview.alpha_composite(disc_frame, (px(1493), px(170)))

def main() -> None:
    if not GUIDE.is_file():
        raise FileNotFoundError(f"missing stack layout guide: {GUIDE}")
    if not RADIO_SOURCE.is_file():
        raise FileNotFoundError(f"missing canonical radio backplate: {RADIO_SOURCE}")
    guide = Image.open(GUIDE)
    if guide.size != (3840, 2160):
        raise ValueError(f"expected 4K stack guide, got {guide.size}")
    radio = Image.open(RADIO_SOURCE).convert("RGBA")
    if radio.size != (3840, 2160):
        raise ValueError(f"expected canonical 4K radio, got {radio.size}")

    source = radio.resize(OUTPUT_SIZE, Image.Resampling.LANCZOS)
    overlay = Image.new("RGBA", OUTPUT_SIZE, (0, 0, 0, 0))
    add_receiver_overlay(source, overlay)
    add_cd_deck(source, overlay)
    write_tga(STACK_OVERLAY, overlay)

    frames = [make_disc_frame(frame, DISC_FRAME_COUNT) for frame in range(DISC_FRAME_COUNT)]
    for index, frame in enumerate(frames):
        write_tga(ART / f"{DISC_FRAME_PREFIX}{index:02d}.tga", frame)

    preview = source.copy()
    preview.alpha_composite(overlay)
    draw_preview_buttons(preview)
    draw_preview_content(preview, frames[0])
    PREVIEW.parent.mkdir(parents=True, exist_ok=True)
    preview.convert("RGB").save(PREVIEW, optimize=True)

    print(f"Stack overlay: {STACK_OVERLAY} ({OUTPUT_SIZE[0]}x{OUTPUT_SIZE[1]}, {STACK_OVERLAY.stat().st_size:,} bytes)")
    print(f"Disc animation: {ART / DISC_FRAME_PREFIX}XX.tga (12 separate 128x128 amber frames)")
    print(f"Composition preview: {PREVIEW}")


if __name__ == "__main__":
    main()
