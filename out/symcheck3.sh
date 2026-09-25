#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
echo "=== app-symbols.map (head) ==="
head -20 "$W/tooling/native/app-symbols.map"
wc -l "$W/tooling/native/app-symbols.map"
echo "=== llvm-pie.elf ==="
ls -la "$W/build/llvm-pie.elf"
file "$W/build/llvm-pie.elf"
readelf -S "$W/build/llvm-pie.elf" 2>/dev/null | grep -cE 'symtab|debug'
echo "=== obj files ==="
find "$W/build/obj" -name '*.o' | head -40
echo "=== runtime-shim ==="
ls "$W/build/runtime-shim" | head
