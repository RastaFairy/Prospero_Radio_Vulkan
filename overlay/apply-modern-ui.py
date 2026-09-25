#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

UPSTREAM_SHA = "33898dd35375c1ae8370da137cfb6941d91c7684"
VERSION = "01.000.014"


def run(*args: str, cwd: Path) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()




def validate_backplate(path: Path) -> None:
    with path.open("rb") as texture:
        header = texture.read(80)
    if len(header) != 80 or header[:12] != bytes((0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A)):
        raise RuntimeError(f"Backplate is not a complete KTX2 container: {path}")
    vk_format, type_size, width, height, depth, layers, faces, mip_levels, supercompression = struct.unpack_from(
        "<9I", header, 12
    )
    if (vk_format, type_size, width, height, depth, layers, faces, mip_levels, supercompression) != (
        37, 1, 3840, 2160, 0, 0, 1, 12, 0
    ):
        raise RuntimeError(f"Backplate must be full-resolution 4K RGBA8 KTX2 with mipmaps: {path}")

def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"Refusing non-deterministic patch in {path}: expected 1 match, found {count}"
        )
    path.write_text(text.replace(old, new), encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: apply-modern-ui.py <upstream-worktree>", file=sys.stderr)
        return 2

    worktree = Path(sys.argv[1]).resolve()
    overlay = Path(__file__).resolve().parent
    if not (worktree / ".git").exists():
        raise SystemExit(f"Not a Git worktree: {worktree}")

    head = run("git", "rev-parse", "HEAD", cwd=worktree)
    if head != UPSTREAM_SHA:
        raise SystemExit(
            f"Unexpected upstream revision: {head}. Expected {UPSTREAM_SHA}. "
            "The modernization overlay is intentionally pinned to one tested base."
        )

    include = worktree / "include" / "radio_app.hpp"
    replace_once(
        include,
        "    static constexpr unsigned CardCount = 4;",
        "    static constexpr unsigned CardCount = 6;",
    )

    cpp = worktree / "src" / "radio_app.cpp"
    replace_once(cpp, "constexpr unsigned kFocusDiscover = 4;", "constexpr unsigned kFocusDiscover = 6;")
    replace_once(cpp, "constexpr unsigned kFocusPlay = 7;", "constexpr unsigned kFocusPlay = 9;")
    replace_once(cpp, "constexpr unsigned kFocusCredits = 8;", "constexpr unsigned kFocusCredits = 10;")

    old_navigation = """        if (key == RADIO_INPUT_CROSS)\n            TogglePlayback();\n        else if (key == RADIO_INPUT_LEFT && (slot & 1U))\n            focus_ = selected_slot_ = slot - 1;\n        else if (key == RADIO_INPUT_RIGHT)\n        {\n            if (!(slot & 1U) && card_stations_[slot + 1] != InvalidStation)\n                focus_ = selected_slot_ = slot + 1;\n            else\n                focus_ = kFocusPlay;\n        }\n        else if (key == RADIO_INPUT_UP)\n        {\n            if (slot >= 2)\n                focus_ = selected_slot_ = slot - 2;\n            else\n            {\n                ChangePage(-1, slot + 2);\n                return;\n            }\n        }\n        else if (key == RADIO_INPUT_DOWN)\n        {\n            if (slot < 2 && card_stations_[slot + 2] != InvalidStation)\n                focus_ = selected_slot_ = slot + 2;\n            else if (page_start_ + CardCount < visible_count_)\n            {\n                ChangePage(1, slot & 1U);\n                return;\n            }\n            else if (view_ == View::Discover)\n                focus_ = kFocusDiscover + slot % 3;\n            else\n                focus_ = kFocusPlay;\n        }"""
    new_navigation = """        if (key == RADIO_INPUT_CROSS)\n            TogglePlayback();\n        else if (key == RADIO_INPUT_LEFT)\n        {\n            if ((slot % 3U) != 0U)\n                focus_ = selected_slot_ = slot - 1U;\n        }\n        else if (key == RADIO_INPUT_RIGHT)\n        {\n            if ((slot % 3U) != 2U && card_stations_[slot + 1U] != InvalidStation)\n                focus_ = selected_slot_ = slot + 1U;\n            else\n                focus_ = kFocusPlay;\n        }\n        else if (key == RADIO_INPUT_UP)\n        {\n            const unsigned row = slot / 3U;\n            if (row > 0U && card_stations_[slot - 3U] != InvalidStation)\n                focus_ = selected_slot_ = slot - 3U;\n            else\n            {\n                ChangePage(-1, slot + 3U);\n                return;\n            }\n        }\n        else if (key == RADIO_INPUT_DOWN)\n        {\n            const unsigned row = slot / 3U;\n            if (row < 1U && card_stations_[slot + 3U] != InvalidStation)\n                focus_ = selected_slot_ = slot + 3U;\n            else if (page_start_ + CardCount < visible_count_)\n            {\n                ChangePage(1, slot % 3U);\n                return;\n            }\n            else if (view_ == View::Discover)\n                focus_ = kFocusDiscover + slot % 3U;\n            else\n                focus_ = kFocusPlay;\n        }"""
    replace_once(cpp, old_navigation, new_navigation)

    for relative in ("assets/ui/main.rml", "assets/ui/styles/app.rcss"):
        source = overlay / relative
        target = worktree / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")

    for relative in ("assets/ui/art/radio_front_4k.ktx2",):
        source = overlay / relative
        validate_backplate(source)
        target = worktree / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    css = (worktree / "assets/ui/styles/app.rcss").read_text(encoding="utf-8")
    sizes = {int(value) for value in re.findall(r"font-size\s*:\s*(\d+)px", css)}
    allowed_sizes = {20, 24, 28, 32, 36, 40, 48}
    bad_sizes = sorted(sizes - allowed_sizes)
    weights = {int(value) for value in re.findall(r"font-weight\s*:\s*(\d+)", css)}
    bad_weights = sorted(weights - {500})
    if bad_sizes or bad_weights:
        raise RuntimeError(
            "Modernized UI requests unsupported bitmap font tuples: "
            f"sizes={bad_sizes}, weights={bad_weights}"
        )

    for unsupported in ("box-shadow", "text-shadow", "transform", "linear-gradient", "radial-gradient"):
        if unsupported in css:
            raise RuntimeError(f"Modernized UI uses unsupported/unsafe renderer feature: {unsupported}")

    param = worktree / "sce_sys" / "param.json"
    data = json.loads(param.read_text(encoding="utf-8"))
    if data.get("titleId") != "PPSA99001":
        raise RuntimeError("Unexpected Title ID; PPSA99001 must never change")
    data["contentVersion"] = VERSION
    param.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    note = overlay / "MODERNIZED_UI.md"
    (worktree / "MODERNIZED_UI.md").write_text(note.read_text(encoding="utf-8"), encoding="utf-8")

    print("Modernization overlay applied successfully.")
    print(f"  Base commit : {UPSTREAM_SHA}")
    print(f"  Version     : {VERSION}")
    print("  Card grid   : 3 columns x 2 rows")
    print("  Navigation  : three-column movement + paged navigation")
    print("  UI profile  : physical-radio skin / 4K mipmapped KTX2 backplate / minimal RmlUi overlay")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
