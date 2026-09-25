#!/usr/bin/env bash
set -u
D="$HOME/.cache/prospero-radio-modernized/deps/PS5_Vulkan"
echo "=== direct memory / heap calls in driver ==="
grep -rn "sceKernelAllocateDirectMemory\|AllocateDirectMemory\|sceKernelMapDirectMemory\|MapDirectMemory\|DIRECT_MEMORY" "$D/src" 2>/dev/null | grep -v Binary | head -20
echo "=== heap/pool size constants ==="
grep -rn -iE "heap.*(size|bytes)|pool.*(size|bytes)|MEMORY_SIZE|DMEM" "$D/src" 2>/dev/null | grep -viE "binary|\.md" | head -20
