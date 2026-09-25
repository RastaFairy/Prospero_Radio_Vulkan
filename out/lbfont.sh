#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
sed -n '230,278p' "$W/src/bitmap_font_engine.cpp"
echo "=== BitmapFontFace ctor (lines 40-80) ==="
sed -n '40,80p' "$W/src/bitmap_font_engine.cpp"
