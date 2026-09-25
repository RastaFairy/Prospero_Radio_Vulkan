#!/usr/bin/env python3
import struct, bisect
from pathlib import Path

CORE = Path("/tmp/core_da.elf")
SYMS = Path("/tmp/syms.txt")

data = CORE.read_bytes()
phoff = struct.unpack_from("<Q", data, 32)[0]
phentsize, phnum = struct.unpack_from("<HH", data, 54)
segs = []
for i in range(phnum):
    p_type, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from("<IIQQQQQQ", data, phoff + i*phentsize)
    if p_type == 1:
        segs.append((vaddr, filesz, offset, flags))

def read_va(va, n):
    for vaddr, filesz, offset, _ in segs:
        if vaddr <= va < vaddr + filesz:
            return data[offset + (va - vaddr): offset + (va - vaddr) + n]
    return None

syms = []
for line in SYMS.read_text().splitlines():
    p = line.split(' ', 2)
    if len(p) == 3:
        try:
            syms.append((int(p[0], 16), p[2]))
        except ValueError:
            pass
syms.sort()
starts = [s[0] for s in syms]

def sym(a):
    if not (0x400000 <= a < 0xe58000):
        return None
    f = a - 0x400000
    i = bisect.bisect_right(starts, f) - 1
    if i >= 0 and f - syms[i][0] < 0x30000:
        return f"{syms[i][1]}+0x{f - syms[i][0]:x}"
    return "text??"

# crash 0xda (proc 0xda): find its registers from klog? assume rsp=rbp known pattern.
# We'll scan BOTH stack segments: find the one containing a qword 0x42e71a (__append return)
STACK_LO, STACK_HI = 0x7e0000000, 0x7f0000000
for vaddr, filesz, offset, flags in segs:
    if not (STACK_LO <= vaddr <= STACK_HI):
        continue
    window = data[offset:offset+filesz]
    for target, label in ((0x42e71a, "__append ret"), (0x42bd4f, "ReadTextureFile ret"), (0x4001a9, "ud2")):
        needle = struct.pack("<Q", target)
        pos = 0
        while True:
            idx = window.find(needle, pos)
            if idx < 0:
                break
            va = vaddr + idx
            print(f"{label} at 0x{va:x}")
            pos = idx + 1

print()
print("=== stack qwords around each __append-ret hit with neighbor symbols ===")
for vaddr, filesz, offset, flags in segs:
    if not (STACK_LO <= vaddr <= STACK_HI):
        continue
    window = data[offset:offset+filesz]
    needle = struct.pack("<Q", 0x42e71a)
    pos = 0
    while True:
        idx = window.find(needle, pos)
        if idx < 0:
            break
        va = vaddr + idx
        print(f"--- frame near __append ret @ 0x{va:x} ---")
        for off in range(va - 0x40, va + 0x80, 8):
            if off < vaddr or off + 8 > vaddr + filesz:
                continue
            q = struct.unpack_from("<Q", data, offset + (off - vaddr))[0]
            note = sym(q) or ("stack" if STACK_LO <= q < STACK_HI else ("sysmod" if 0x800000000 <= q < 0x900000000 else ""))
            print(f"0x{off:012x}: 0x{q:016x} {note}")
        pos = idx + 1
