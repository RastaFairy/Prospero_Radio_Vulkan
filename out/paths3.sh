#!/usr/bin/env bash
set -u
W="$HOME/.cache/prospero-radio-modernized/ProsperoRadio-33898dd35375c1ae8370da137cfb6941d91c7684"
grep -nE 'PATH|path' "$W/src/radio_service.cpp" | grep -iE '"/|data|temp|define' | head -15
