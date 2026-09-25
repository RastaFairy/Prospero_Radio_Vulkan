# BUILD-FIX6 — Vulkan integration compile repair

This revision prepares the ProsperoRadio modernization overlay for the next real build. It preserves the established Vulkan/UI changes and repairs the integration points that were still preventing the pinned upstream build from consuming them.

## Repairs

1. Vulkan include/archive/import paths are exported relative to the generated upstream worktree, matching the original build script's path validation.
2. Prebuilt PS5 Vulkan `.so`/`.sprx` SCE import stubs are accepted by the upstream import-stub/link stage; the original Opus stub generation remains intact.
3. The PS5_Vulkan dependency is pinned by default to commit `085aac6a9e42c0d6337660e7148eb8990604052a`, while `PS5_VULKAN_REF` remains an explicit override.
4. UI shader sources are resolved from the launcher overlay directory instead of a non-existent `overlay/` directory inside the generated upstream checkout.
5. PIC-only driver archives are excluded from the title's static link set.
6. Mesa's `u_thread`, `anon_file`, and `os_file` utility objects are compiled into the ProsperoRadio build; the PS5 Vulkan shared build can leave these unresolved, but the final title link cannot.
7. Vulkan archives are linked as a dedicated whole-archive group, with the PS5 C++ runtime archives grouped beside them.
8. The generated application loop is Vulkan-only; SDL no longer creates or presents through a software renderer.
9. `VK_KHR_surface` is explicitly enabled together with `VK_KHR_display`.
10. The added renderer sources now satisfy the repository's SPDX/header lint policy.

## Validation note

The repair package is statically checked here, but the actual PS5 SDK/Mesa build and console execution must still be run on the configured WSL/Linux build host.


11. PS5_Vulkan PSBC bootstrap is patched in-place before compilation to generate the Mesa files omitted by its standalone build script but required by the pinned ps5-opengl 0.3.0 tree, including `u_format_gen.h`, `u_format_pack.h`, `u_format_table.c`, `format_srgb.c`, compiler builtins, shader stats, Vulkan struct casts, PM4 headers, and the gfx10 format table.
12. The patch is idempotent and refuses to apply if the expected upstream marker is absent, preventing silent edits to an incompatible PS5_Vulkan revision.

## Build failure addressed by FIX6

Observed on the WSL2 build host:
`src/util/format/u_formats.h:33:10: fatal error: 'util/format/u_format_gen.h' file not found`

The pinned PS5_Vulkan `tools/build-psbc-ps5.sh` generates the NIR/ACO compiler sources but omitted the Mesa format/compiler generated files. The pinned `ps5-opengl` build script explicitly generates these files before `make`, so FIX6 mirrors that required generation step inside the ProsperoRadio bootstrap.
