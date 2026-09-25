# BUILD-FIX16

## Change

FIX16 targets the PSBC compiler warning counts observed in the real build:
- PS5 PSBC: 119 warnings in the rebuilt sources.
- Host PIC PSBC: 114 warnings.

## Implementation

`overlay/tools/patch-ps5-vulkan-warning-policy.py` patches the pinned PS5_Vulkan build scripts to add a Mesa-style warning policy at the Makefile level, without editing Mesa/opengnm source files solely to silence diagnostics.

Suppressed high-noise diagnostics:
- `-Wno-missing-field-initializers`
- `-Wno-format-truncation`
- `-Wno-nonnull-compare`
- `-Wno-unknown-pragmas`
- `-Wno-unused-parameter`
- C++: `-Wno-non-virtual-dtor`

A version stamp forces the affected PSBC objects to rebuild whenever the policy changes. The build also extracts remaining warning categories from compiler output so any residual diagnostics are visible.

## Validation

- `python3 -m py_compile overlay/apply-vulkan.py overlay/tools/patch-ps5-vulkan-warning-policy.py` — passed.
- `bash -n overlay/setup-ps5-vulkan.sh` — passed.
- Synthetic PS5 PSBC and host-PIC build-script patching — passed.
- Synthetic patching repeated twice — idempotent.

## Current status

The warning count is not claimed to be zero until the patched build is executed in the user's WSL2 environment.
The strong undefined Vulkan entry-point blocker from FIX15 remains separate.
