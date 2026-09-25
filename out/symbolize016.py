#!/usr/bin/env python3
import struct, bisect
from pathlib import Path

ELF = Path("/mnt/d/prospero_modern/out/PPSA99001-v016-llvm-pie.elf")
import subprocess
nm = subprocess.run(["nm", "-C", "--defined-only", str(ELF)], capture_output=True, text=True).stdout
syms = []
for line in nm.splitlines():
    p = line.split(' ', 2)
    if len(p) == 3 and p[1] in ('t', 'T', 'w', 'W'):
        try:
            syms.append((int(p[0], 16), p[2]))
        except ValueError:
            pass
syms.sort()
starts = [s[0] for s in syms]

BASE = 0x400000
addrs = [0x40016b, 0x401758, 0x404b6a, 0x4054b8, 0x423479, 0x423758, 0x400190]
for a in addrs:
    f = a - BASE
    i = bisect.bisect_right(starts, f) - 1
    if i >= 0 and f - syms[i][0] < 0x40000:
        sa, name = syms[i]
        print(f"0x{a:x}  ->  {name}  (+0x{f - sa:x})")
    else:
        print(f"0x{a:x}  ->  ?? (file 0x{f:x})")

# disassemble around the two innermost frames
for lo, hi, label in ((0x1750, 0x1790, "0x401758 area"), (0x4b40, 0x4b80, "0x404b6a area"), (0x5490, 0x54d0, "0x4054b8 area")):
    print(f"--- disasm {label} ---")
    d = subprocess.run(["objdump", "-d", "--start-address=" + hex(lo), "--stop-address=" + hex(hi), str(ELF)],
                       capture_output=True, text=True).stdout
    for line in d.splitlines():
        if line.strip().startswith(("%", "0")) or ":" in line:
            print(line)
