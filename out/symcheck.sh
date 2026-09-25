#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ls -la "$W/build/eboot.elf"
file "$W/build/eboot.elf" | head -2
readelf -S "$W/build/eboot.elf" 2>/dev/null | grep -E 'symtab|debug|comment' | head
echo "--- nm sample ---"
nm -C "$W/build/eboot.elf" 2>/dev/null | wc -l
