#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ELF="$W/build/llvm-pie.elf"
objdump -d --start-address=0x502b --stop-address=0x5120 "$ELF" 2>/dev/null | tail -55
echo "=== error targets ==="
objdump -d --start-address=0x54ba --stop-address=0x54d0 "$ELF" 2>/dev/null | tail -8
objdump -d --start-address=0x5527 --stop-address=0x5560 "$ELF" 2>/dev/null | tail -14
