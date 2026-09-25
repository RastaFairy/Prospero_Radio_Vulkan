#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
wc -l "$W/src/bitmap_font_engine.cpp"
echo "=== glyph insertion sites ==="
grep -n "LoadFontFace\|glyphs\[\|glyphs.insert\|emplace\|operator\[\]\|while\|for (" "$W/src/bitmap_font_engine.cpp" | head -40
