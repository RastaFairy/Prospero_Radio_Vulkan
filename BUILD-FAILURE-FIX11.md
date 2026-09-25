# BUILD FAILURE FIX11 — duplicate PSBC compatibility symbols

## Failure
The final ProsperoRadio link failed with duplicate strong symbols:
- `__assert`: `src/main.cpp` vs `libpsbc_support.ps5.a(psbc_ps5_shims.ps5.o)`
- `localtime_r`: `src/sqlite_compat.cpp` vs `libpsbc_support.ps5.a(psbc_ps5_shims.ps5.o)`

## Root cause
The PS5_Vulkan support archive intentionally supplies these compatibility functions for Mesa/PSBC. ProsperoRadio still had older title-local fallback definitions for the same symbols. Linking the support archive with `--whole-archive` therefore creates a genuine ODR/ELF duplicate.

## Fix
1. Remove the title-local `__assert` definition from `src/main.cpp` during the Vulkan overlay.
2. Remove the title-local `localtime_r` stub from `src/sqlite_compat.cpp` during the Vulkan overlay.
3. Keep the PS5_Vulkan implementations in `libpsbc_support.ps5.a`; those implementations are the functional compatibility boundary for Mesa/PSBC.
4. Add `overlay/tools/check-vulkan-link-duplicates.py` and invoke it before the final PS5 title link. It audits strong symbols across title objects, Vulkan whole-archive inputs, and extra Mesa objects and fails early with the exact owners if a duplicate appears.

## Validation
- `python3 -m py_compile overlay/apply-vulkan.py`
- `python3 -m py_compile overlay/tools/check-vulkan-link-duplicates.py`
- `bash -n build.sh`
- Synthetic `nm --format=posix` duplicate test passed: the audit detects a strong symbol defined by both an object and an archive.
- The compatibility-removal transform was applied to the uploaded `src` tree and leaves no title-local `__assert` or `localtime_r` definitions.

## Important
Do not claim the final PS5 title link or package succeeds until the user's real WSL/PS5 toolchain reaches `eboot.bin` and package validation.
