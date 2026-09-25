#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
echo "=== page refs in Radio-20.fnt ==="
grep -oE 'file="[^"]*"' "$W/assets/ui/fonts/lvgl-bitmap/multilingual/Radio-20.fnt" | head -6
grep -oE '<common[^>]*' "$W/assets/ui/fonts/lvgl-bitmap/multilingual/Radio-20.fnt" | head -2
echo "=== page files on disk ==="
ls -la "$W/assets/ui/fonts/lvgl-bitmap/multilingual/" | head -12
echo "=== RCSS font-family usage ==="
grep -rn "font-family" "$W/assets/ui/" 2>/dev/null | head -10
echo "=== LoadFontFace calls in main.cpp ==="
sed -n '315,345p' "$W/src/main.cpp"
