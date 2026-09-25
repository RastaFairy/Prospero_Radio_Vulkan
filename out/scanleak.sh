#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ls "$W" | head -40
echo "=== find src dirs ==="
find "$W" -maxdepth 2 -type d -name 'src' 2>/dev/null | head
find "$W" -maxdepth 2 -name 'radio_service*' 2>/dev/null | head
