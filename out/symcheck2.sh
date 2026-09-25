#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
ls "$W/build" | head -30
echo "--- obj count ---"
find "$W/build" -name '*.o' 2>/dev/null | wc -l
find "$W/build" -name '*.a' 2>/dev/null | head
echo "--- any map ---"
find "$W" -name '*.map' 2>/dev/null | grep -v \.git | head
echo "--- debug sections ---"
readelf -S "$W/build/eboot.elf" | grep -E 'Name|\.' | head -30
