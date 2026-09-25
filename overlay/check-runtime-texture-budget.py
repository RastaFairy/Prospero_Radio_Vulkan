#!/usr/bin/env python3
"""Validate the full-resolution KTX2 backplate and its bounded upload size."""

from pathlib import Path
import struct
import sys


OVERLAY = Path(__file__).resolve().parent
RML = OVERLAY / "assets/ui/main.rml"
SOURCE = OVERLAY / "assets/ui/art/radio_front_4k.tga"
KTX2 = OVERLAY / "assets/ui/art/radio_front_4k.ktx2"
KTX2_IDENTIFIER = bytes((0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A))
VK_FORMAT_R8G8B8A8_UNORM = 37
MAX_CONTAINER_BYTES = 48 * 1024 * 1024
STAGING_BYTES = 2 * 1024 * 1024


def fail(message: str) -> None:
    raise SystemExit(message)


def main() -> int:
    rml = RML.read_text(encoding="utf-8")
    if 'src="art/radio_front_4k.ktx2"' not in rml:
        fail(f"{RML} must use the full-resolution KTX2 backplate")

    with SOURCE.open("rb") as source:
        source_header = source.read(18)
    if len(source_header) != 18 or source_header[2] != 2 or source_header[16] != 32:
        fail(f"{SOURCE} must remain an uncompressed 32-bit full-resolution source TGA")
    source_size = struct.unpack_from("<HH", source_header, 12)

    file_size = KTX2.stat().st_size
    with KTX2.open("rb") as texture:
        header = texture.read(80)
    if file_size < 80 or len(header) != 80 or header[:12] != KTX2_IDENTIFIER:
        fail(f"{KTX2} is not a valid KTX2 container")

    vk_format, type_size, width, height, depth, layers, faces, mip_levels, supercompression = struct.unpack_from(
        "<9I", header, 12
    )
    if vk_format != VK_FORMAT_R8G8B8A8_UNORM or type_size != 1:
        fail(f"{KTX2} must use Vulkan VK_FORMAT_R8G8B8A8_UNORM")
    if (width, height) != source_size or (width, height) != (3840, 2160):
        fail(f"{KTX2} must retain the complete 3840x2160 source, got {width}x{height}")
    if depth != 0 or layers != 0 or faces != 1 or supercompression != 0:
        fail(f"{KTX2} must be a single uncompressed 2D Vulkan texture")
    if mip_levels != max(width, height).bit_length():
        fail(f"{KTX2} must contain the complete mip pyramid, expected {max(width, height).bit_length()} levels")
    if file_size > MAX_CONTAINER_BYTES:
        fail(f"{KTX2.name} exceeds the {MAX_CONTAINER_BYTES // (1024 * 1024)} MiB container budget")

    index_end = 80 + mip_levels * 24
    dfd_offset, dfd_length, kvd_offset, kvd_length = struct.unpack_from("<4I", header, 48)
    min_payload_offset = max(index_end, dfd_offset + dfd_length, kvd_offset + kvd_length)
    total_gpu_bytes = 0
    payload_ranges = []
    level_width, level_height = width, height
    with KTX2.open("rb") as texture:
        for level in range(mip_levels):
            texture.seek(80 + level * 24)
            index = texture.read(24)
            if len(index) != 24:
                fail(f"{KTX2.name} has a truncated level index at mip {level}")
            offset, byte_length, uncompressed_length = struct.unpack("<3Q", index)
            expected_length = level_width * level_height * 4
            if offset < min_payload_offset or offset % 4 != 0:
                fail(f"{KTX2.name} has an invalid payload offset for mip {level}")
            if byte_length != expected_length or uncompressed_length != expected_length:
                fail(f"{KTX2.name} has an invalid byte count for mip {level}")
            if offset + byte_length > file_size:
                fail(f"{KTX2.name} mip {level} extends beyond the file")
            total_gpu_bytes += byte_length
            payload_ranges.append((offset, offset + byte_length))
            level_width = max(1, level_width // 2)
            level_height = max(1, level_height // 2)

    payload_ranges.sort()
    for previous, current in zip(payload_ranges, payload_ranges[1:]):
        if previous[1] > current[0]:
            fail(f"{KTX2.name} has overlapping mip payloads")

    print(
        f"Runtime KTX2 validated: {KTX2.name} ({width}x{height}, {mip_levels} mips, "
        f"{total_gpu_bytes / 1048576:.2f} MiB GPU image, {STAGING_BYTES / 1048576:.0f} MiB upload staging)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
