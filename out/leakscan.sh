#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
for f in radio_service icy_metadata ogg_stream radio_hls radio_playlist radio_catalog_store; do
  echo "=== src/$f.cpp ==="
  grep -nE 'push_back|resize|reserve|insert\(|append\(|new |malloc|realloc' "$W/src/$f.cpp" 2>/dev/null | head -12
done
echo "=== dist contents ==="
ls -la "$W/dist" 2>/dev/null | head
