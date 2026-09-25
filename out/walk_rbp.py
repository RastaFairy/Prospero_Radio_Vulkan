#!/usr/bin/env python3
import struct
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

def read_va(va, n):
    for vaddr, filesz, offset, _ in segs:
        if vaddr <= va < vaddr + filesz:
            return data[offset + (va - vaddr): offset + (va - vaddr) + n]
    return None

TEXT_LO, TEXT_HI = 0x400000, 0xe58000
rsp = 0x7eeffb1e0

print("=== raw stack from rsp upward (0x100 bytes) ===")
block = read_va(rsp, 0x100)
for off in range(0, 0x100, 16):
    va = rsp + off
    row = block[off:off+16]
    q0, q1 = struct.unpack_from("<Q", row, 0), struct.unpack_from("<Q", row, 8)
    ann = []
    for q in (q0, q1):
        if TEXT_LO <= q < TEXT_HI:
            ann.append(f"text 0x{q:x}")
        elif 0x7eeff8000 <= q < 0x7eeffc000:
            ann.append("stack")
        elif 0x800000000 <= q < 0x880000000:
            ann.append("sysmod")
    print(f"0x{va:012x}: {' '.join(f'{b:02x}' for b in row[:8])} {' '.join(f'{b:02x}' for b in row[8:])}  {' '.join(ann)}")

print()
print("=== rbp chain walk ===")
rbp = rsp
for depth in range(12):
    vals = read_va(rbp, 16)
    if not vals:
        print(f"rbp 0x{rbp:x}: not captured")
        break
    saved_rbp, ret = struct.unpack_from("<QQ", vals)
    ann = f"ret=0x{ret:x}" + (f" (text)" if TEXT_LO <= ret < TEXT_HI else "")
    print(f"depth {depth}: rbp=0x{rbp:012x} saved_rbp=0x{saved_rbp:012x} {ann}")
    if saved_rbp <= rbp or not (0x7eeff8000 <= saved_rbp < 0x7eeffc000):
        break
    rbp = saved_rbp

print()
print("=== dump frames between rsp and rsp+0x90 annotated ===")
# print qwords 0x00..0x90 with symbol resolution using llvm-pie.elf symtab if available
import bisect
syms = []
symfile = Path("/tmp/syms.txt")
if symfile.exists():
    for line in symfile.read_text().splitlines():
        p = line.split(' ', 2)
        if len(p) == 3:
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
syms.sort()
starts = [s[0] for s in syms]
def sym(a):
    f = a - 0x400000
    i = bisect.bisect_right(starts, f) - 1
    if i >= 0 and f - syms[i][0] < 0x40000:
        return f"{syms[i][1]}+0x{f - syms[i][0]:x}"
    return "??"
for off in range(0, 0x90, 8):
    va = rsp + off
    q = struct.unpack_from("<Q", block, off)[0]
    note = sym(q) if TEXT_LO <= q < TEXT_HI else ""
    print(f"[rsp+0x{off:02x}] 0x{q:016x} {note}")
