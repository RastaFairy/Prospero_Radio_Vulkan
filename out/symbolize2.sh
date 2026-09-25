#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ELF="$W/build/llvm-pie.elf"
nm -C --defined-only "$ELF" 2>/dev/null | awk '$2 ~ /[tTwW]/ {print $1, $2, $3}' | sort > /tmp/syms.txt
wc -l /tmp/syms.txt
python3 - <<'PYEOF'
syms = []
with open('/tmp/syms.txt') as f:
    for line in f:
        parts = line.rstrip('\n').split(' ', 2)
        if len(parts) == 3:
            try:
                syms.append((int(parts[0], 16), parts[1], parts[2]))
            except ValueError:
                pass
syms.sort()
import bisect
addrs = [0x4001a9, 0x40016b, 0x42b83a, 0x429e97, 0x42a203, 0x42a41e, 0xc36aa4,
         0xc96396, 0xc96409, 0xc95015, 0xcafdc8, 0xbcfb25, 0xcc8936, 0xcc7573,
         0xccd962, 0xcccdbf, 0xcc89bc, 0xcccf80, 0xcccd98, 0xcd243a, 0xbe0d35,
         0xba4402, 0xba41d2, 0x42353a, 0x423189, 0x42bd4f, 0x42bfab, 0xc3a154,
         0xc99a46, 0xc99aa9, 0xc3bff3, 0xc29ec0, 0xbee652, 0xbe77d4, 0xbe2150,
         0xbf25dd, 0xbd073b, 0xbd0758, 0xba75f4, 0x423701, 0x42e714, 0x42e6c0]
starts = [s[0] for s in syms]
for a in addrs:
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0:
        sa, typ, name = syms[i]
        print(f"0x{a:06x}  ->  {name}  (+0x{a - sa:x})")
    else:
        print(f"0x{a:06x}  ->  ??")
PYEOF
