#!/usr/bin/env bash
# Prospero Radio Vulkan - compatibility entry point for the canonical build.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-ffpfsc}"

exec bash "$SCRIPT_DIR/build.sh" "$MODE"
