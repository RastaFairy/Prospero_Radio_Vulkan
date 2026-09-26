#!/usr/bin/env python3
"""Build the hybrid radio backplate and review previews.

The orthographic flat fascia covers the old controls while the photographed
rear chassis remains visible around it. Outputs include a 4K source TGA and a
Vulkan-ready RGBA8 KTX2 candidate. Requires Pillow.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
ART = ROOT / "overlay/assets/ui/art"
CONTROLS = ROOT / "overlay/assets/ui/controls"
SOURCE_PHOTO = DOCS / "radio-front-head-preview.png"
SOURCE_FASCIA = ART / "radio_front_4k.tga"
KTX2_TEMPLATE = ART / "radio_front_4k.ktx2"
RUNTIME_TGA = ART / "radio_front_hybrid_4k.tga"
RUNTIME_KTX2 = ART / "radio_front_hybrid_4k.ktx2"
MANIFEST = CONTROLS / "manifest.json"
BASE = DOCS / "radio-front-hybrid-base.png"
PROPOSAL = DOCS / "radio-front-hybrid-proposal.png"
PREVIEW = DOCS / "radio-front-hybrid-preview.png"
SCALE = 2
SOURCE_FACE = (86.0, 294.0, 1748.0, 492.0)
TARGET_FACE_WIDTH = 1850.0
FACE_SCALE = TARGET_FACE_WIDTH / SOURCE_FACE[2]
TARGET_CORE = (
    (1920.0 - TARGET_FACE_WIDTH) / 2,
    (1080.0 - SOURCE_FACE[3] * FACE_SCALE) / 2,
    TARGET_FACE_WIDTH,
    SOURCE_FACE[3] * FACE_SCALE,
)


def transformed_rect(rect: list[float] | tuple[float, ...]) -> tuple[int, int, int, int]:
    x, y, width, height = rect
    source_x, source_y, _, _ = SOURCE_FACE
    target_x, target_y, _, _ = TARGET_CORE
    return tuple(
        round(value * SCALE)
        for value in (
            target_x + (x - source_x) * FACE_SCALE,
            target_y + (y - source_y) * FACE_SCALE,
            target_x + (x + width - source_x) * FACE_SCALE,
            target_y + (y + height - source_y) * FACE_SCALE,
        )
    )


def place_frame(
    canvas: Image.Image,
    atlas_path: Path,
    frame: dict,
    target_rect: list[float],
) -> None:
    atlas = Image.open(atlas_path).convert("RGBA")
    frame_image = atlas.crop((frame["x"], frame["y"], frame["x"] + frame["width"], frame["y"] + frame["height"]))
    x0, y0, x1, y1 = transformed_rect(target_rect)
    frame_image = frame_image.resize((x1 - x0, y1 - y0), Image.Resampling.LANCZOS)
    canvas.alpha_composite(frame_image, (x0, y0))


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    path = Path("C:/Windows/Fonts") / name
    if not path.is_file():
        path = Path("C:/Windows/Fonts/arial.ttf")
    return ImageFont.truetype(str(path), size * SCALE)


def draw_sample_station(canvas: Image.Image, screen_rect: list[float]) -> None:
    draw = ImageDraw.Draw(canvas)
    x, y, _right, _bottom = transformed_rect(screen_rect)
    left = x + round(35 * FACE_SCALE * SCALE)
    top = y + round(27 * FACE_SCALE * SCALE)
    draw.text((left, top), "MANGORADIO", font=font(31, True), fill=(244, 190, 118, 255))
    draw.text(
        (left, top + 70),
        "Germany / Rheinland-Pfalz  ·  English, German",
        font=font(17),
        fill=(226, 166, 88, 255),
    )
    draw.text(
        (left, top + 112),
        "music  ·  MP3 128 kbps",
        font=font(17),
        fill=(188, 126, 57, 255),
    )
    # The source capture did not provide a current track title or album art;
    # leave those areas empty instead of drawing placeholder content.


def write_hybrid_tga(image: Image.Image) -> None:
    rgba = image.convert("RGBA")
    width, height = rgba.size
    header = bytearray(18)
    header[2] = 2
    struct.pack_into("<HHHHBB", header, 8, 0, 0, width, height, 32, 0x28)
    RUNTIME_TGA.write_bytes(bytes(header) + rgba.tobytes("raw", "BGRA"))


def write_hybrid_ktx2(image: Image.Image) -> None:
    identifier = bytes((0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A))
    template = bytearray(KTX2_TEMPLATE.read_bytes())
    if template[:12] != identifier:
        raise ValueError(f"Invalid KTX2 template: {KTX2_TEMPLATE}")
    fields = struct.unpack_from("<9I", template, 12)
    width, height, levels = fields[2], fields[3], fields[7]
    if image.size != (width, height) or levels != max(width, height).bit_length():
        raise ValueError("KTX2 template dimensions or mip chain do not match hybrid backplate")

    mip = image.convert("RGBA")
    for level in range(levels):
        index_offset = 80 + level * 24
        payload_offset, byte_length, uncompressed_length = struct.unpack_from(
            "<3Q", template, index_offset
        )
        payload = mip.tobytes("raw", "RGBA")
        if len(payload) != byte_length or len(payload) != uncompressed_length:
            raise ValueError(f"KTX2 payload size mismatch at mip {level}")
        template[payload_offset:payload_offset + byte_length] = payload
        if level + 1 < levels:
            mip = mip.resize((max(1, mip.width // 2), max(1, mip.height // 2)), Image.Resampling.LANCZOS)
    RUNTIME_KTX2.write_bytes(template)


def main() -> None:
    if not MANIFEST.is_file():
        raise SystemExit("Generate the control atlases first: python tools/build_radio_controls.py")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    photo = Image.open(SOURCE_PHOTO).convert("RGBA")
    fascia_art = Image.open(SOURCE_FASCIA).convert("RGBA")
    if photo.size != (3840, 2160) or fascia_art.size != (3840, 2160):
        raise ValueError("The photo and flat fascia source must both be 3840x2160")

    # Scale the orthographic fascia uniformly and seat it across the full
    # receiver face. Keep the photographed rear chassis visible around it for
    # physical depth; the old controls themselves stay fully covered.
    target_x, target_y, target_width, target_height = TARGET_CORE
    sx, sy, sw, sh = SOURCE_FACE
    px0, py0, px1, py1 = (round(value * SCALE) for value in (sx, sy, sx + sw, sy + sh))
    face = fascia_art.crop((px0, py0, px1, py1)).resize(
        (round(target_width * SCALE), round(target_height * SCALE)), Image.Resampling.LANCZOS
    )
    face_mask = Image.new("L", face.size, 0)
    ImageDraw.Draw(face_mask).rounded_rectangle(
        (0, 0, face.width - 1, face.height - 1), radius=round(18 * FACE_SCALE * SCALE), fill=255
    )

    # Let the room's lamp warm the materials slightly, but preserve the neutral
    # metal and leave the display untouched for dynamic runtime content.
    light = Image.new("RGBA", face.size, (0, 0, 0, 0))
    ImageDraw.Draw(light).ellipse(
        (face.width * 0.48, -face.height * 0.55, face.width * 1.25, face.height * 0.85),
        fill=(255, 173, 91, 10),
    )
    light = light.filter(ImageFilter.GaussianBlur(145 * SCALE))
    content_mask = face_mask.copy()
    screen = transformed_rect(manifest["screen"]["rect"])
    render_x = round(target_x * SCALE)
    render_y = round(target_y * SCALE)
    screen_local = (
        screen[0] - render_x, screen[1] - render_y,
        screen[2] - render_x, screen[3] - render_y,
    )
    ImageDraw.Draw(content_mask).rectangle(screen_local, fill=0)
    light.putalpha(ImageChops.multiply(light.getchannel("A"), content_mask))
    face = Image.alpha_composite(face, light)

    base = photo.copy()
    shadow_mask = Image.new("L", photo.size, 0)
    ImageDraw.Draw(shadow_mask).rounded_rectangle(
        (render_x, render_y + face.height - 3 * SCALE,
         render_x + face.width, render_y + face.height + 8 * SCALE),
        radius=round(12 * FACE_SCALE * SCALE), fill=48
    )
    shadow = Image.new("RGBA", photo.size, (13, 9, 6, 0))
    shadow.putalpha(shadow_mask.filter(ImageFilter.GaussianBlur(14 * SCALE)))
    base = Image.alpha_composite(base, shadow)
    base.paste(face, (render_x, render_y), face_mask)
    base.save(BASE, optimize=True)
    write_hybrid_tga(base)
    write_hybrid_ktx2(base)

    example = base.copy()
    volume = manifest["volume"]
    volume_frame = next(item for item in volume["levels"] if item["value"] == 65)
    place_frame(example, CONTROLS / volume["file"], volume_frame, volume["logicalTargetRect"])

    buttons = manifest["buttons"]["controls"]
    favorites = buttons["favorites"]
    place_frame(
        example,
        CONTROLS / manifest["buttons"]["file"],
        favorites["frames"]["selected_focus"],
        favorites["targetRect"],
    )

    tuner = manifest["tuner"]
    place_frame(
        example,
        CONTROLS / tuner["file"],
        tuner["frames"]["next_focus"],
        tuner["targetRect"],
    )
    draw_sample_station(example, manifest["screen"]["contentRect"])
    example.save(PROPOSAL, optimize=True)

    example.resize((1920, 1080), Image.Resampling.LANCZOS).save(PREVIEW, optimize=True)

    print(f"Hybrid backplate: {RUNTIME_KTX2} ({RUNTIME_KTX2.stat().st_size:,} bytes)")
    print(f"Authoring source: {RUNTIME_TGA} ({RUNTIME_TGA.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
