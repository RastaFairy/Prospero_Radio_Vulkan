#!/usr/bin/env python3
# ProsperoRadio Vulkan link-integrity check.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

STRONG_TYPES = set("ABCDRSTV")


def read_symbols(nm: str, path: str) -> list[str]:
    result = subprocess.run(
        [nm, "-g", "--defined-only", "--format=posix", path],
        check=True,
        capture_output=True,
        text=True,
    )
    names: list[str] = []
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name, symbol_type = parts[0], parts[1]
        if symbol_type in STRONG_TYPES:
            names.append(name)
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit strong duplicate symbols before PS5 title linking")
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objects", nargs="*", default=[])
    parser.add_argument("--archives", nargs="*", default=[])
    args = parser.parse_args()

    owners: dict[str, list[str]] = defaultdict(list)
    inputs = [p for p in [*args.objects, *args.archives] if p]
    for path in inputs:
        if Path(path).suffix not in {".o", ".a"}:
            continue
        for symbol in read_symbols(args.nm, path):
            owners[symbol].append(path)

    duplicates = {name: paths for name, paths in owners.items() if len(set(paths)) > 1}
    if not duplicates:
        print("Vulkan link symbol audit: no strong duplicate symbols")
        return 0

    print("Vulkan link symbol audit: duplicate strong symbols detected", file=sys.stderr)
    for name in sorted(duplicates):
        print(f"  {name}", file=sys.stderr)
        for path in sorted(set(duplicates[name])):
            print(f"    {path}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
