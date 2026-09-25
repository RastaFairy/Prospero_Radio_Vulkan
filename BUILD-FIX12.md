# BUILD FIX12 — robust linker-audit bootstrap

## Failure observed in FIX11
The PowerShell build stopped inside `overlay/apply-vulkan.py` before rebuilding the title:

```text
RuntimeError: Could not locate final ProsperoRadio linker invocation
```

## Root cause
`patch_link_duplicate_audit()` searched for a literal string containing `\\n` rather than matching the actual newline/command structure of `tools/build.sh`. In addition, the audit was invoked before `patch_target_build_driver()`, which is the function that creates the `vulkan_archives` and `extra_objects` shell arrays used by the audit.

This made FIX11 fail during overlay application instead of during compilation.

## FIX12
1. Locate the final `prospero-lld -T "$native/ps5-pie.ld"` invocation using a structural regular expression rather than brittle full-line text matching.
2. Require exactly one matching linker invocation.
3. Apply `patch_target_build_driver()` before installing the duplicate-symbol audit, so `vulkan_archives` and `extra_objects` are guaranteed to exist in the generated build script.
4. Generate the audit command using explicit newline joining, preserving valid Bash line continuations.
5. Keep the existing strong-symbol audit limited to title objects, extra Mesa objects and Vulkan whole-archive inputs, avoiding false positives from ordinary lazy static archives.

## Validation performed
- `python3 -m py_compile overlay/apply-vulkan.py`
- synthetic upstream-shaped `tools/build.sh` passed both patch functions;
- generated linker audit placement verified;
- generated `tools/build.sh` passed `bash -n`;
- real `nm` duplicate-symbol test detects a duplicate strong symbol between two ELF objects;
- single-object audit reports no duplicate strong symbols.

## Current state
The real PS5 toolchain has already demonstrated successful construction of:
- PSBC PS5 compiler and support archive;
- adapted PS5 OpenGL SDK and host compiler;
- Mesa 26.2.0 runtime;
- PS5 Vulkan driver and signed `libvulkan.so.1` test modules;
- the ProsperoRadio Vulkan renderer compilation up to final title linking.

The final title/package build has **not** yet been declared successful. The next run should first verify that the overlay itself completes and that the pre-link symbol audit executes before `prospero-lld`.
