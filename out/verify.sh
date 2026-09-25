#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ELF="$W/build/llvm-pie.elf"
echo "=== ReadWholeFile full disasm (vaddr 0x2aaf0..0x2ad50) ==="
objdump -d --start-address=0x2aaf0 --stop-address=0x2ad50 "$ELF" 2>/dev/null | grep -E "call|cmp|movabs|ja |jbe|jmp" | head -40
