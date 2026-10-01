#!/usr/bin/env python3
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

"""Post-build precision gate for the ProsperoRadio package.

Run against the assembled package. It mirrors the shipped reader and layout
rules so a regression in any of them fails the gate before flashing:

  V  version coherence: apply-vulkan VERSION == param.json == RML stamp ==
     runtime banner inside eboot.bin
  P  persistence helper: named ELF is present and targets x86-64 ELF64
  R  RML: XML structure and unique ids; every static id the C++ touches
     exists; every image source is packaged; 21 volume frames stacked
  C  CSS cascade: classes used by the RML are defined; atlas geometry rules
     match manifest-hybrid.json; the LAST same-specificity rule for each atlas
     class must carry that class's own rect (no later base rule overriding it)
  G  geometry containment: glass surfaces + EQ stay inside the display rect
  T  TGA mirror of the shipped reader: uncompressed truecolor, bpp/alpha
     combinations the reader accepts, orientation both ways, exact payload or
     a structurally valid TGA 2.0 footer, sane dimensions, and split frames
     sized exactly as the manifest core rects

Exit code 0 only when no check fails. Warnings never fail the build.
"""
from __future__ import annotations

import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SURFACES = (
    "screen-home",
    "screen-list",
    "screen-genres",
    "screen-aux",
    "screen-barrido",
    "screen-eq",
    "search-panel",
)
ATLAS_CLASSES = ("volume-frame-img", "tuner-frame-img") + tuple(
    f"btn-rect-{name}"
    for name in ("home", "radio", "favorites", "genres", "search", "settings", "play-pause")
)
FOOTER_SIGNATURE = b"TRUEVISION-XFILE.\0"


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.warnings: list[str] = []
        self.passes: list[str] = []

    def fail(self, check: str, detail: str) -> None:
        self.failures.append(f"[FAIL] {check}: {detail}")

    def warn(self, check: str, detail: str) -> None:
        self.warnings.append(f"[WARN] {check}: {detail}")

    def ok(self, check: str, detail: str) -> None:
        self.passes.append(f"[ ok ] {check}: {detail}")


def parse_rules(css: str) -> list[tuple[str, dict[str, str]]]:
    """Ordered rule list: (selector, {property: value}). Grouped selectors
    register each name; the order encodes the cascade for later overrides."""
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)  # strip comments
    rules: list[tuple[str, dict[str, str]]] = []
    for match in re.finditer(r"([^{}]+)\{([^}]*)\}", css):
        properties: dict[str, str] = {}
        for declaration in match.group(2).split(";"):
            if ":" not in declaration:
                continue
            name, value = declaration.split(":", 1)
            properties[name.strip()] = value.strip()
        for selector in match.group(1).split(","):
            selector = selector.strip()
            if selector.startswith((".", "#")):
                rules.append((selector, properties))
    return rules


def effective_rect(ordered_rules, selector: str):
    """Last-wins geometry for a selector across the ordered cascade."""
    left = top = width = height = None
    for sel, properties in ordered_rules:
        if sel != selector:
            continue
        for key in ("left", "top", "width", "height"):
            if key in properties:
                if key == "left":
                    left = properties[key]
                elif key == "top":
                    top = properties[key]
                elif key == "width":
                    width = properties[key]
                else:
                    height = properties[key]
    if None in (left, top, width, height):
        return None
    try:
        return tuple(
            float(str(value).removesuffix("px")) for value in (left, top, width, height)
        )
    except ValueError:
        return None


def rect_of(rules_by_selector: dict[str, dict[str, str]], selector: str):
    properties = rules_by_selector.get(selector)
    if not properties:
        return None
    try:
        return tuple(
            float(properties[key].removesuffix("px"))
            for key in ("left", "top", "width", "height")
        )
    except (KeyError, ValueError):
        return None


def check_versions(pkg: Path, overlay: Path, report: Report) -> None:
    apply_text = (overlay / "apply-vulkan.py").read_text(encoding="utf-8")
    version = re.search(r'^VERSION = "(\d+\.\d+\.\d+)"', apply_text, re.M)
    if not version:
        report.fail("version", "apply-vulkan.py has no VERSION")
        return
    expected = version.group(1)

    param = json.loads((pkg / "sce_sys/param.json").read_text(encoding="utf-8"))
    if param.get("contentVersion") != expected:
        report.fail("version", f"param.json={param.get('contentVersion')} != VERSION={expected}")
    else:
        report.ok("version", f"param.json == {expected}")

    rml = (pkg / "assets/ui/main.rml").read_text(encoding="utf-8")
    stamps = set(re.findall(r"\d{2}\.\d{3}\.\d{3}", rml))
    if stamps - {expected}:
        report.fail("version", f"RML carries foreign stamps: {sorted(stamps - {expected})}")
    elif expected in stamps:
        report.ok("version", f"RML stamp == {expected}")
    else:
        report.warn("version", "RML carries no version stamp")

    eboot = (pkg / "eboot.bin").read_bytes()
    banners = set(re.findall(rb"fork (\d{2}\.\d{3}\.\d{3})", eboot))
    if banners - {expected.encode()}:
        report.fail("version", f"runtime banner(s) {banners} != {expected}")
    elif banners:
        report.ok("version", f"runtime banner == {expected}")
    else:
        report.fail("version", "runtime banner missing from eboot.bin")


def check_persistence_payload(pkg: Path, report: Report) -> None:
    payload = pkg / "assets/payload/ProsperoRadioDataBridge.elf"
    if not payload.is_file():
        report.fail("payload", "named persistence ELF is missing from the assembled package")
        return
    data = payload.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        report.fail("payload", "persistence asset is not a complete ELF")
        return
    elf_class, data_encoding = data[4], data[5]
    machine = int.from_bytes(data[18:20], "little") if data_encoding == 1 else -1
    if elf_class != 2 or data_encoding != 1 or machine != 62:
        report.fail(
            "payload",
            f"expected little-endian ELF64 x86-64 (class={elf_class}, data={data_encoding}, machine={machine})",
        )
    else:
        report.ok("payload", f"named ELF64 x86-64 helper packaged ({len(data)} bytes)")


def check_rml(pkg: Path, overlay: Path, report: Report) -> None:
    rml = (pkg / "assets/ui/main.rml").read_text(encoding="utf-8")
    try:
        root = ET.fromstring(rml)
    except ET.ParseError as error:
        report.fail("rml", f"XML/RML parse error: {error}")
        return
    report.ok("rml", "XML/RML structure parsed")

    ids_in_document = [element.get("id") for element in root.iter() if element.get("id")]
    seen_ids: set[str] = set()
    duplicate_ids: set[str] = set()
    for uid in ids_in_document:
        if uid in seen_ids:
            duplicate_ids.add(uid)
        seen_ids.add(uid)
    if duplicate_ids:
        report.fail("rml", f"duplicate ids: {sorted(duplicate_ids)}")
    else:
        report.ok("rml", f"all {len(ids_in_document)} ids are unique")
    ids = set(ids_in_document)
    app_src = (overlay / "src/radio_app.cpp").read_text(encoding="utf-8")
    used = set(
        re.findall(
            r'(?:SetText|SetVisible|SetClass|SetPixelProperty|SetProperty)\(\s*document_\s*,\s*"([^"]+)"',
            app_src,
        )
    )
    dynamic_prefixes = ("volume_frame_", "tuner_", "btn-")
    missing = {
        uid
        for uid in used
        if uid not in ids and not any(uid.startswith(p) for p in dynamic_prefixes)
    }
    if missing:
        report.fail("rml", f"C++ references ids absent from main.rml: {sorted(missing)}")
    else:
        report.ok("rml", f"all {len(used)} static ids referenced by C++ exist")

    missing_frames = [i for i in range(21) if f"volume_frame_{i:02d}" not in ids]
    if missing_frames:
        report.fail("rml", f"volume frames missing from RML: {missing_frames}")
    else:
        report.ok("rml", "21 volume frames stacked")

    sources = [element.get("src") for element in root.iter() if element.get("src")]
    absent = [src for src in sources if not (pkg / "assets/ui" / src).is_file()]
    if absent:
        report.fail("rml", f"<img> sources missing in package: {absent[:4]}")
    else:
        report.ok("rml", f"all {len(sources)} <img> sources packaged")


def check_css(pkg: Path, overlay: Path, report: Report) -> None:
    app_css = (pkg / "assets/ui/styles/app.rcss").read_text(encoding="utf-8")
    controls_css = (pkg / "assets/ui/styles/controls.rcss").read_text(encoding="utf-8")
    rml = (pkg / "assets/ui/main.rml").read_text(encoding="utf-8")

    ordered = parse_rules(app_css + "\n" + controls_css)
    rules_by_selector: dict[str, dict[str, str]] = {}
    for selector, properties in ordered:
        rules_by_selector.setdefault(selector, {}).update(properties)

    rml_classes: set[str] = set()
    for value in re.findall(r'class="([^"]+)"', rml):
        rml_classes.update(value.split())
    undefined = sorted(
        c for c in rml_classes
        if not any(
            re.search(rf"\.{re.escape(c)}(?![A-Za-z0-9_-])", selector)
            for selector in rules_by_selector
        )
    )
    if undefined:
        report.warn("css", f"RML classes without a CSS rule (semantic markers?): {undefined}")
    else:
        report.ok("css", f"all {len(rml_classes)} RML classes defined")

    # atlas geometry must match the manifest, and the LAST same-specificity
    # rule for each atlas class must be that class's own rect
    manifest = json.loads(
        (overlay / "assets/ui/controls/manifest-hybrid.json").read_text(encoding="utf-8")
    )
    rects = {
        "volume-frame-img": manifest["volume"]["logicalTargetRect"],
        "tuner-frame-img": manifest["tuner"]["targetRect"],
    }
    for control, info in manifest["buttons"]["controls"].items():
        rects[f"btn-rect-{control}"] = info["targetRect"]

    effective: dict[str, dict[str, str]] = {}
    for selector, properties in ordered:
        effective.setdefault(selector, {}).update(properties)

    failures_here = 0
    for atlas_class in ATLAS_CLASSES:
        rect = effective_rect(ordered, f".{atlas_class}")
        expected = rects.get(atlas_class)
        if rect is None:
            report.fail("css", f"atlas geometry rule missing: .{atlas_class}")
            failures_here += 1
            continue
        if expected:
            drift = max(abs(a - b) for a, b in zip(rect, expected))
            if drift > 0.5:
                report.fail(
                    "css",
                    f".{atlas_class} effective rect {tuple(rect)} != manifest {tuple(expected)} "
                    f"(drift {drift:.2f}px)",
                )
                failures_here += 1
    if failures_here == 0:
        report.ok("css", "atlas geometry matches the manifest under the cascade")

    if ".frame-img" in rules_by_selector and "width" in rules_by_selector[".frame-img"]:
        report.fail("css", ".frame-img carries geometry and would override control rects")


def check_geometry(pkg: Path, report: Report) -> None:
    app_css = (pkg / "assets/ui/styles/app.rcss").read_text(encoding="utf-8")
    rules_by_selector: dict[str, dict[str, str]] = {}
    for selector, properties in parse_rules(app_css):
        rules_by_selector.setdefault(selector, {}).update(properties)
    display = rect_of(rules_by_selector, "#display")
    if display is None:
        report.fail("geometry", "#display rect missing")
        return
    _, _, dw, dh = display
    for surface in SURFACES:
        rect = rect_of(rules_by_selector, f"#{surface}")
        if rect is None:
            report.fail("geometry", f"#{surface} has no rect (class/id mismatch?)")
            continue
        left, top, width, height = rect
        if left < 0 or top < 0 or left + width > dw + 0.5 or top + height > dh + 0.5:
            report.fail(
                "geometry",
                f"#{surface} escapes the glass ({left},{top},{width},{height} vs {dw}x{dh})",
            )
    eq = rect_of(rules_by_selector, "#eq")
    if eq and eq[1] + eq[3] > dh + 0.5:
        report.fail("geometry", f"#eq escapes the glass (top {eq[1]} + height {eq[3]} > {dh})")
    else:
        report.ok("geometry", "glass surfaces and EQ inside #display")


def check_tga(pkg: Path, overlay: Path, report: Report) -> None:
    """Mirror of the shipped reader rules plus manifest dimension checks."""
    manifest = json.loads(
        (overlay / "assets/ui/controls/manifest-hybrid.json").read_text(encoding="utf-8")
    )
    expected_dims: dict[str, tuple[int, int]] = {}
    volume = manifest["volume"]
    vg = volume["extrudedGutterPx"]
    cw, ch = volume["cell"]
    for level in volume["levels"]:
        expected_dims[f"volume_frame_{level['index']:02d}.tga"] = (cw - 2 * vg, ch - 2 * vg)
    focus = volume["focusOverlay"]
    expected_dims["volume_focus_ring.tga"] = (focus["width"] - 2 * vg, focus["height"] - 2 * vg)
    tuner = manifest["tuner"]
    tg = tuner["extrudedGutterPx"]
    tcw, tch = tuner["cell"]
    for state, frame in tuner["frames"].items():
        expected_dims[f"tuner_{state}.tga"] = (tcw - 2 * tg, tch - 2 * tg)
    buttons = manifest["buttons"]
    bg = buttons["extrudedGutterPx"]
    bcw, bch = buttons["cell"]
    for control, info in buttons["controls"].items():
        for state, frame in info["frames"].items():
            expected_dims[f"btn_{control}_{state}.tga"] = (bcw - 2 * bg, bch - 2 * bg)

    failures = 0
    checked = 0
    for path in sorted((pkg / "assets/ui").rglob("*.tga")):
        relative = path.relative_to(pkg / "assets/ui")
        data = path.read_bytes()
        checked += 1
        if len(data) < 18 or data[2] != 2:
            report.fail("tga", f"{relative}: not an uncompressed truecolor TGA")
            failures += 1
            continue
        bpp, descriptor = data[16], data[17]
        alpha = descriptor & 0x0F
        width = data[12] | (data[13] << 8)
        height = data[14] | (data[15] << 8)
        valid_format = (
            (bpp == 32 and alpha in (0, 8)) or (bpp == 24 and alpha == 0)
        ) and 0 < width <= 4096 and 0 < height <= 4096
        payload = width * height * (bpp // 8)
        body = len(data) - 18
        has_footer = data[-18:] == FOOTER_SIGNATURE
        size_ok = body == payload or (has_footer and body == payload + 26)
        name = relative.name
        if name in expected_dims:
            size_ok = size_ok and (width, height) == expected_dims[name]
        if not valid_format:
            report.fail("tga", f"{relative}: bpp {bpp} alpha {alpha} rejected by reader")
            failures += 1
        elif not size_ok:
            report.fail("tga", f"{relative}: payload {body} != {payload} and no valid footer")
            failures += 1
    if failures == 0:
        report.ok("tga", f"all {checked} TGAs pass the reader rules")
    frames = pkg / "assets/ui/controls/frames"
    count = len(list(frames.glob("*.tga"))) if frames.is_dir() else 0
    if count != 62:
        report.fail("tga", f"controls/frames has {count} TGAs (expected 62)")
    else:
        report.ok("tga", "62 atlas frames packaged")


def main() -> int:
    pkg = (
        Path(sys.argv[1]).resolve()
        if len(sys.argv) > 1
        else Path(__file__).resolve().parents[2] / "out/PPSA99001"
    )
    overlay = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
    if not pkg.is_dir():
        print(f"package directory not found: {pkg}", file=sys.stderr)
        return 2

    report = Report()
    check_versions(pkg, overlay, report)
    check_persistence_payload(pkg, report)
    check_rml(pkg, overlay, report)
    check_css(pkg, overlay, report)
    check_geometry(pkg, report)
    check_tga(pkg, overlay, report)

    for line in report.passes + report.warnings + report.failures:
        print(line)
    print(
        f"\n{len(report.passes)} passed, {len(report.warnings)} warnings, "
        f"{len(report.failures)} failures"
    )
    return 1 if report.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
