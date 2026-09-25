# BUILD-FAILURE-FIX6

Observed build failure:
`src/util/format/u_formats.h:33:10: fatal error: 'util/format/u_format_gen.h' file not found`

Root cause:
The pinned `PS5_Vulkan` commit `085aac6a9e42c0d6337660e7148eb8990604052a` bootstraps its PSBC work copy but does not generate the Mesa `src/util/format` and related generated sources required by the pinned `ps5-opengl` 0.3.0 compiler tree.

Reference behavior:
The pinned `ps5-opengl` build script generates:
- `src/util/format/u_format_gen.h`
- `src/util/format/u_format_pack.h`
- `src/util/format/u_format_table.c`
- `src/util/format_srgb.c`
- compiler builtin tables
- shader stats
- Vulkan struct cast metadata
- AMD PM4 generated headers
- the gfx10 format table

FIX6 applies these generation commands to the local PS5_Vulkan checkout immediately after checkout and before `tools/build-psbc-ps5.sh` runs. The change is idempotent and guarded by an exact upstream marker.
