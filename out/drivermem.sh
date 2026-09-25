#!/usr/bin/env bash
set -u
D="$HOME/.cache/prospero-radio-modernized/deps/PS5_Vulkan"
echo "=== big pool / malloc in driver init ==="
grep -rn -iE "pool_size|POOL_BYTES|POOL_SIZE|reserve|giant|slab|arena" "$D/src"/*.cpp 2>/dev/null | grep -viE "//|small" | head -15
echo "=== device creation allocations ==="
grep -rn "malloc\|calloc\|operator new\|new \[" "$D/src/ps5vk_device.cpp" 2>/dev/null | head -10
ls "$D/src" | head -30
