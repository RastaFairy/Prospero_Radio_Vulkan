#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ELF="$W/build/llvm-pie.elf"
echo "=== addr2line -i with corrected vaddrs (runtime-0x400000) ==="
for a in 0x1a9 0x2b83a 0x29e97 0x2a203 0x2a41e 0x2bd4f 0x2bfab 0x2e6c0 0x2e714 0x23701; do
  echo "--- $a ---"
  addr2line -f -C -i -e "$ELF" "$a"
done
echo "=== disasm around LoadKtx2Texture+0xae5 (vaddr 0x2b835) ==="
objdump -d --start-address=0x2b7f0 --stop-address=0x2b860 "$ELF" 2>/dev/null | tail -25
