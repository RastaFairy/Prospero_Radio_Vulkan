#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
echo "=== write paths in app ==="
grep -rn '"/data\|"/temp\|/app0\|fopen.*"w' "$W/src"/*.cpp 2>/dev/null | grep -v '//' | head -12
echo "=== main.cpp head ==="
head -40 "$W/src/main.cpp"
