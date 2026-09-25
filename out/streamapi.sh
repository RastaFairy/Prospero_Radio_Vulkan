#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
echo "=== StreamFile API ==="
find "$W/vendor" -name "StreamFile.h" | head -2
sed -n '1,60p' "$(find "$W/vendor" -name "StreamFile.h" | head -1)" 2>/dev/null | grep -E "class|bool|Open|StreamFile|virtual" | head -12
echo "=== FileInterface API ==="
grep -nE "virtual.*(Open|Read|Length|Close|Seek)" "$(find "$W/vendor" -name "FileInterface.h" | head -1)" | head -8
echo "=== current RunApp body (patched main.cpp) ==="
sed -n "$(grep -n 'RunApp' "$W/src/main.cpp" | head -2 | tail -1 | cut -d: -f1),+40p" "$W/src/main.cpp"
