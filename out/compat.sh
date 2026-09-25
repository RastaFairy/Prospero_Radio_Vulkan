#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
echo "=== how the compat interface is used in main.cpp ==="
grep -n "RenderInterfaceCompatibility\|GetCompatibility\|SetRenderInterface\|RenderGeometry\|ps5_vulkan\|Ps5Vulkan" "$W/src/main.cpp" | head -20
echo "=== RmlUi compat header: RenderGeometry semantics ==="
find "$W" -name "RenderInterfaceCompatibility.h" -not -path "*/\.git/*" | head -2
grep -n "RenderGeometry\|RenderGeometryHandle\|ReleaseCompiledGeometry" "$(find "$W" -name "RenderInterfaceCompatibility.h" -not -path "*/\.git/*" | head -1)" 2>/dev/null | head -20
