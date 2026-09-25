#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
grep -n 'FAVORITES_TEMP_PATH\|define FAVORITES\|FAVORITES_PATH' "$W/src/radio_service.cpp" | head -5
grep -n 'int main' "$W/src/main.cpp"
sed -n "$(grep -n 'int main' "$W/src/main.cpp" | head -1 | cut -d: -f1),+25p" "$W/src/main.cpp"
