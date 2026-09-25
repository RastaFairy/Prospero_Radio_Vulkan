#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ELF="$W/build/llvm-pie.elf"
objdump -d --start-address=0x4ef0 --stop-address=0x5010 "$ELF" 2>/dev/null | tail -70
