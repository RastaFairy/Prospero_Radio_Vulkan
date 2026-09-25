#!/usr/bin/env bash
set -u
D="$HOME/.cache/prospero-radio-modernized/deps/PS5_Vulkan"
grep -n "memory_type\|kMapProtection\|kLargeAlignment\|PROT_\|memory_alignment\|kDirectMemoryType" "$D/src/demo_renderer.cpp" "$D/src/diagnostics.cpp" 2>/dev/null | grep -E "=|constexpr|const int|define" | head -20
echo "=== sceKernelAllocateDirectMemory decl ==="
sed -n '20,35p' "$D/src/demo_renderer.cpp"
