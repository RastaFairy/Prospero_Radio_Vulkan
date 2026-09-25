#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
echo "=== font assets ==="
find "$W/assets" -iname "*.fnt" -o -iname "*font*" 2>/dev/null | head -12
echo "=== sizes ==="
find "$W/assets" -iname "*.fnt" -exec ls -la {} \; 2>/dev/null | head
echo "=== char element counts ==="
for f in $(find "$W/assets" -iname "*.fnt" 2>/dev/null); do
  echo "$f : $(grep -c '<char ' "$f" 2>/dev/null) char elems, $(head -c 60 "$f" | tr -d '\0')"
done
echo "=== rcss font references ==="
grep -rn "font-family\|font-face\|fnt" "$W/assets/ui/styles/"*.rcss 2>/dev/null | head -8
