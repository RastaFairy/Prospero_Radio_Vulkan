#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
nm -C --defined-only "$W/build/llvm-pie.elf" 2>/dev/null | awk '$2 ~ /^[tTwW]$/ {print $1, $2, $3}' | sort > /tmp/syms.txt
wc -l /tmp/syms.txt
python3 /tmp/walk_da.py
