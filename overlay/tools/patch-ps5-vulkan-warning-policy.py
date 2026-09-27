#!/usr/bin/env python3
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make the pinned PSBC builds expose every warning and fail until fixed."""
from __future__ import annotations

import re
import sys
from pathlib import Path

STRICT_MAKEFILE = "prospero-warning-check.mak"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count == 1:
        return text.replace(old, new, 1)
    if count == 0 and new in text:
        return text
    raise RuntimeError(f"{label}: expected one match, found {count}")


def create_strict_makefile(root: Path) -> None:
    path = root / "tools" / STRICT_MAKEFILE
    path.write_text(
        "# Keep PSBC diagnostics visible and make every warning a build failure.\n"
        "override CFLAGS := $(filter-out -Wno-%,$(CFLAGS))\n"
        "override CXXFLAGS := $(filter-out -Wno-%,$(CXXFLAGS))\n"
        "override CFLAGS += -DHAVE_FUNC_ATTRIBUTE_UNUSED=1 -DPROSPERO_PSBC_STANDALONE=1 -Werror\n"
        "override CXXFLAGS += -DHAVE_FUNC_ATTRIBUTE_UNUSED=1 -DPROSPERO_PSBC_STANDALONE=1 -Werror\n",
        encoding="utf-8",
    )


def patch_ps5_build(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "WARNING_POLICY_VERSION=" in text or "CFLAGS += -w" in text or "CXXFLAGS += -w" in text:
        raise RuntimeError(f"legacy warning suppression still present in {path}")
    old = '-f "$root/tooling/psbc/support.mk" ' + "\\" + "\n"
    new = (
        old
        + f'        -f "$root/tools/{STRICT_MAKEFILE}" '
        + "\\"
        + "\n"
    )
    text = replace_once(text, old, new, "PS5 PSBC strict-warning make include")
    patch_anchor = 'python3 "$root/tooling/psbc/patch-subpass-input.py" "$tree"\n'
    patch_hook = (
        patch_anchor
        + '\n# Apply warning fixes after upstream compiler patches so their anchors remain valid.\n'
        + 'python3 "$root/tools/prospero-psbc-warning-fixes.py" "$tree"\n'
    )
    text = replace_once(
        text,
        patch_anchor,
        patch_hook,
        "PSBC source warning fixes after upstream patches",
    )
    if re.search(r"(?<!\S)-w(?!\S)|-Wno-[A-Za-z0-9-]+", text):
        raise RuntimeError(f"warning suppression flag found in {path}")
    path.write_text(text, encoding="utf-8")


def patch_driver_build(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "WARNING_POLICY_VERSION=" in text or "CFLAGS += -w" in text or "CXXFLAGS += -w" in text:
        raise RuntimeError(f"legacy warning suppression still present in {path}")
    old = '-f "$root/tooling/psbc/Makefile.opengnm-psbc-host-pic" -j"$(nproc)"'
    new = (
        '-f "$root/tooling/psbc/Makefile.opengnm-psbc-host-pic" '
        f'-f "$root/tools/{STRICT_MAKEFILE}" -j"$(nproc)"'
    )
    text = replace_once(text, old, new, "host PIC strict-warning make include")
    if re.search(r"(?<!\S)-w(?!\S)|-Wno-[A-Za-z0-9-]+", text):
        raise RuntimeError(f"warning suppression flag found in {path}")
    path.write_text(text, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: patch-ps5-vulkan-warning-policy.py <PS5_Vulkan root> <source-fixer>",
            file=sys.stderr,
        )
        return 2
    root = Path(sys.argv[1]).resolve()
    source_fixer = Path(sys.argv[2]).resolve()
    if not source_fixer.is_file():
        raise SystemExit(f"missing PSBC source fixer: {source_fixer}")
    fixer_target = root / "tools" / "prospero-psbc-warning-fixes.py"
    fixer_target.write_bytes(source_fixer.read_bytes())
    create_strict_makefile(root)
    patch_ps5_build(root / "tools/build-psbc-ps5.sh")
    patch_driver_build(root / "tools/build-driver.sh")
    print("PS5_Vulkan: PSBC warnings visible; -Wno-* removed and -Werror enabled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
