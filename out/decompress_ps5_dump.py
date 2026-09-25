from pathlib import Path
import sys


def decompress_block(block: bytes) -> bytes:
    src = memoryview(block)
    pos = 0
    out = bytearray()
    while pos < len(src):
        token = src[pos]
        pos += 1

        literal_len = token >> 4
        if literal_len == 15:
            while True:
                extra = src[pos]
                pos += 1
                literal_len += extra
                if extra != 255:
                    break
        out.extend(src[pos:pos + literal_len])
        pos += literal_len
        if pos == len(src):
            break

        offset = src[pos] | (src[pos + 1] << 8)
        pos += 2
        if offset == 0 or offset > len(out):
            raise ValueError(f"invalid LZ4 match offset {offset} at {pos - 2}")

        match_len = (token & 0x0F) + 4
        if (token & 0x0F) == 15:
            while True:
                extra = src[pos]
                pos += 1
                match_len += extra
                if extra != 255:
                    break
        for _ in range(match_len):
            out.append(out[-offset])
    return bytes(out)


def decompress_frame(data: bytes) -> bytes:
    if data[:4] != b"\x04\x22\x4d\x18":
        raise ValueError("not an LZ4 frame")
    flg = data[4]
    pos = 7  # magic, FLG, BD, header checksum
    if flg & 0x08:
        pos += 8
    if flg & 0x01:
        pos += 4

    out = bytearray()
    while True:
        block_size = int.from_bytes(data[pos:pos + 4], "little")
        pos += 4
        if block_size == 0:
            break
        is_raw = bool(block_size & 0x80000000)
        block_size &= 0x7FFFFFFF
        block = data[pos:pos + block_size]
        if len(block) != block_size:
            raise ValueError("truncated LZ4 block")
        out.extend(block if is_raw else decompress_block(block))
        pos += block_size
        if flg & 0x10:
            pos += 4  # block checksum

    if flg & 0x04:
        pos += 4  # content checksum
    if pos != len(data):
        raise ValueError(f"unexpected trailing bytes: {len(data) - pos}")
    return bytes(out)


for arg in sys.argv[1:]:
    src = Path(arg)
    dst = src.with_suffix(src.suffix + ".elf")
    decoded = decompress_frame(src.read_bytes())
    dst.write_bytes(decoded)
    print(f"{src.name}: {len(decoded)} bytes -> {dst}")
    print(f"  magic={decoded[:4]!r}, elf_class={decoded[4] if decoded[:4] == b'\x7fELF' else 'n/a'}")
