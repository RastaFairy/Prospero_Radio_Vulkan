#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ELF="$W/build/llvm-pie.elf"
echo "=== loop 0x1600-0x1760 (inlined ctor copy in LoadFontFace) ==="
objdump -d --start-address=0x1600 --stop-address=0x1760 "$ELF" 2>/dev/null | tail -60
echo "=== font registration in app ==="
grep -rn "LoadFontFace\|fonts/" "$W/src/radio_app.cpp" "$W/src/main.cpp" 2>/dev/null | head -10
