#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ELF="$W/build/llvm-pie.elf"
echo "=== ReadTextureFile first 0x80 bytes (vaddr 0x2bd00) ==="
objdump -d --start-address=0x2bd00 --stop-address=0x2bd60 "$ELF" 2>/dev/null | tail -25
echo "=== vector<unsigned char>::__append (vaddr 0x2e6a0..0x2e720) ==="
objdump -d --start-address=0x2e6a0 --stop-address=0x2e720 "$ELF" 2>/dev/null | tail -30
