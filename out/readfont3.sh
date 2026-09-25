#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
sed -n '275,385p' "$W/src/bitmap_font_engine.cpp"
echo "=== header glyph/map types ==="
grep -n "FontGlyphs\|FontKerning\|robin_hood\|using.*Map\|max_size\|reserve" "$W/src/bitmap_font_engine.cpp" | head
