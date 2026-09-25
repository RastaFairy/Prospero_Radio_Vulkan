#!/usr/bin/env bash
set -u
D="$HOME/.cache/prospero-radio-modernized/deps/PS5_Vulkan"
echo "=== demo_renderer DMEM usage (constants) ==="
sed -n '380,425p' "$D/src/demo_renderer.cpp"
echo "=== any unmap/free calls in driver ==="
grep -rn "UnmapDirectMemory\|FreeDirectMemory\|sceKernelMapDirectMemory" "$D/src" | head -8
