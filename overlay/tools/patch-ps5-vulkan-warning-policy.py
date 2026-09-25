#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Callable

VERSION = "mesa-warning-policy-v5"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def replace_once_regex(
    text: str,
    pattern: str,
    repl: str | Callable[[re.Match[str]], str],
    label: str,
    flags: int = 0,
) -> str:
    matches = list(re.finditer(pattern, text, flags))
    if len(matches) != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {len(matches)}")
    match = matches[0]
    replacement = repl(match) if callable(repl) else repl
    return text[: match.start()] + replacement + text[match.end() :]


def guard_stale_warning_policy(text: str, label: str) -> None:
    versions = sorted(set(re.findall(r"mesa-warning-policy-v\d+", text)))
    stale = [version for version in versions if version != VERSION]
    if stale:
        joined = ", ".join(stale)
        raise RuntimeError(
            f"{label}: stale warning policy detected ({joined}); "
            f"PS5_Vulkan must be reset to commit before patching"
        )


def warning_policy_block(variable: str, audit_variable: str) -> str:
    return (
        f"WARNING_POLICY_VERSION=\"{VERSION}\"\n"
        f"warning_policy=\"$work/{variable}.mak\"\n"
        f"warning_policy_stamp=\"$work/{variable}.version\"\n"
        f"if [[ ! -f $warning_policy_stamp || $(cat \"$warning_policy_stamp\") != \"$WARNING_POLICY_VERSION\" ]]; then\n"
        "    find \"$tree\" -type f \\( -name '*.o' -o -name '*.a' \\) -delete\n"
        "    printf '%s\\n' \"$WARNING_POLICY_VERSION\" > \"$warning_policy_stamp\"\n"
        "fi\n"
        f"cat > \"$warning_policy\" <<'EOF_WARNING_POLICY_{audit_variable.upper()}\'\n"
        f"# PSBC is a pinned third-party Mesa/opengnm build. Default CI output is clean;\n"
        f"# set {audit_variable}=1 to audit upstream diagnostics without suppression.\n"
        f"ifeq ($({audit_variable}),1)\n"
        "else\n"
        "CFLAGS += -w\n"
        "CXXFLAGS += -w\n"
        "endif\n"
        f"EOF_WARNING_POLICY_{audit_variable.upper()}\n"
    )


def patch_ps5_build(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    guard_stale_warning_policy(text, "PS5 PSBC build")
    if VERSION in text:
        return

    marker = 'tree="$work/third_party/opengnm-psbc"\n'
    policy = "\n" + warning_policy_block("psbc-warning-policy", "PS5VK_PSBC_WARNING_AUDIT")
    text = replace_once(text, marker, marker + policy, "PS5 PSBC tree marker")

    old = '-f "$root/tooling/psbc/support.mk" ' + chr(92) + '\n'
    new = (
        '-f "$root/tooling/psbc/support.mk" ' + chr(92) + '\n'
        '        -f "$warning_policy" ' + chr(92) + '\n'
    )
    text = replace_once(text, old, new, "PS5 PSBC make invocation")

    old = "warnings=$(grep -c 'warning:' \"$log\" || true)\n"
    text = replace_once(
        text,
        old,
        old + "warning_categories=$(grep -oE '\\[-W[^]]+\\]' \"$log\" | sort -u | tr '\\n' ' ' || true)\n",
        "PS5 warning count",
    )

    # Provenance formatting is intentionally optional: earlier overlay stages may
    # already have rewritten this block. It must never make the warning policy
    # application fail after the actual make invocation was patched successfully.
    pattern = re.compile(r'^\s*echo "this run: \$compiled sources compiled, \$warnings compiler warnings"\s*$', re.M)
    match = pattern.search(text)
    if match:
        replacement = (
            '    echo "this run: $compiled sources compiled, $warnings compiler warnings (${warning_categories:-none})"\n'
            '    echo "warning policy: $WARNING_POLICY_VERSION (set PS5VK_PSBC_WARNING_AUDIT=1 to show vendor warnings)"'
        )
        text = text[:match.start()] + replacement + text[match.end():]
    else:
        text += (
            '\n# Warning policy v4 provenance is emitted by the caller when the canonical\n'
            '# upstream provenance line is absent or has already been rewritten.\n'
        )
    path.write_text(text, encoding="utf-8")


def patch_driver_build(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    guard_stale_warning_policy(text, "PS5 Vulkan driver build")
    if VERSION in text:
        return

    work_marker = 'work="$root/build/driver"\n'
    policy = "\n" + warning_policy_block("psbc-warning-policy-host", "PS5VK_PSBC_WARNING_AUDIT")
    text = replace_once(text, work_marker, work_marker + policy, "driver work marker")

    old = '-f "$root/tooling/psbc/Makefile.opengnm-psbc-host-pic" -j"$(nproc)"'
    new = '-f "$root/tooling/psbc/Makefile.opengnm-psbc-host-pic" -f "$warning_policy" -j"$(nproc)"'
    text = replace_once(text, old, new, "host PIC PSBC make invocation")

    pattern = re.compile(r'^echo "libpsbc\.pic\.a: .*?warnings, \$\(stat -c %s "\$host_psbc"\) bytes"\s*$', re.M)
    match = pattern.search(text)
    if match:
        replacement = (
            "pic_warnings=$(grep -c 'warning:' \"$pic_log\" || true)\n"
            "pic_warning_categories=$(grep -oE '\\[-W[^]]+\\]' \"$pic_log\" | sort -u | tr '\\n' ' ' || true)\n"
            "echo \"libpsbc.pic.a: $(grep -cE ' -c [^ ]+\\.(c|cpp) ' \"$pic_log\" || true) sources compiled, $pic_warnings warnings (${pic_warning_categories:-none}), $(stat -c %s \"$host_psbc\") bytes\""
        )
        text = text[:match.start()] + replacement + text[match.end():]
    path.write_text(text, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-ps5-vulkan-warning-policy.py <PS5_Vulkan root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    patch_ps5_build(root / "tools/build-psbc-ps5.sh")
    patch_driver_build(root / "tools/build-driver.sh")
    print(f"PS5_Vulkan: applied {VERSION}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
