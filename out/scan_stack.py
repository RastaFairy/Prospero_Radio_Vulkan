#!/usr/bin/env python3
import struct, sys
from pathlib import Path

path = Path("out/captured_coredumps/prosperocore-1790369650-0x0000010e-eboot.bin.prosperodmp.elf")
data = path.read_bytes()
phoff = struct.unpack_from("<Q", data, 32)[0]
phentsize, phnum = struct.unpack_from("<HH", data, 54)
segs = []
for i in range(phnum):
    p_type, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from("<IIQQQQQQ", data, phoff + i*phentsize)
    if p_type == 1:
        segs.append((vaddr, filesz, offset, flags))

targets = {
    0x8001D3CF0: "requested new capacity (2x corrupt cap)",
    0x4000E9E78: "corrupt capacity value",
}
print("=== scanning all captured windows for crash values ===")
for vaddr, filesz, offset, flags in segs:
    window = data[offset:offset+filesz]
    for ta, label in targets.items():
        needle = struct.pack("<Q", ta)
        pos = 0
        while True:
            idx = window.find(needle, pos)
            if idx < 0:
                break
            va = vaddr + idx
            print(f"FOUND {label}: 0x{ta:x} at VA 0x{va:x} (flags={flags:#x})")
            pos = idx + 1

# dump context around each stack hit
print()
print("=== context around hits ===")
for vaddr, filesz, offset, flags in segs:
    if not (0x7ee000000 <= vaddr <= 0x7ff000000):
        continue
    window = data[offset:offset+filesz]
    for ta in targets:
        needle = struct.pack("<Q", ta)
        pos = 0
        while True:
            idx = window.find(needle, pos)
            if idx < 0:
                break
            va = vaddr + idx
            lo, hi = max(vaddr, va - 0x60), min(vaddr + filesz, va + 0x60)
            print(f"--- around 0x{va:x} ---")
            for off in range(lo, hi, 16):
                row = data[offset + (off - vaddr): offset + (off - vaddr) + 16]
                hexs = ' '.join(f"{b:02x}" for b in row[:8]) + '  ' + ' '.join(f"{b:02x}" for b in row[8:])
                q0 = struct.unpack_from('<Q', row, 0)[0]
                q1 = struct.unpack_from('<Q', row, 8)[0]
                print(f"0x{off:012x}: {hexs}   | {q0:#x} {q1:#x}")
            pos = idx + 1
