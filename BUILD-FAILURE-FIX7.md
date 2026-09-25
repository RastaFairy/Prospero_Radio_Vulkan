# BUILD-FAILURE-FIX7

Observed build failure:
`tools/build-driver.sh: line 297: .../PS5_Vulkan/build/host/ps5-native-tool: No such file or directory`

What had already succeeded:
- PS5 PSBC compiler archive
- PSBC support archive
- PS5 OpenGL SDK reconstruction and host compiler
- Mesa 26.2.0 fetch/verification
- Vulkan runtime
- PS5 driver host/PS5 object compilation and archive preparation

Root cause:
`PS5_Vulkan/tools/build-driver.sh` assumes its repository-local `build/host/ps5-native-tool` exists for FSELF sign/extract operations. That executable is provided by the ProsperoRadio native application tooling (`tooling/native/*.cpp`), not by the pinned PS5_Vulkan repository.

FIX7:
`overlay/setup-ps5-vulkan.sh` now builds the ProsperoRadio `ps5-native-tool` from:
- `tooling/native/native_app_builder.cpp`
- `tooling/native/self_container.cpp`
- `tooling/native/elf_object.cpp`
- `tooling/native/sce_module_writer.cpp`
plus the cached host zlib archive,
and stages the resulting executable at:
`$CACHE_ROOT/PS5_Vulkan/build/host/ps5-native-tool`

The pinned PS5_Vulkan checkout remains otherwise unchanged. The bridge is idempotent and runs immediately before `tools/build-driver.sh`.


FIX8: Vulkan header staging corrected. The PS5 driver now builds; title compilation failed because `.local/vulkan/include/vulkan_core.h` referenced missing `vk_video/*` headers after setup script copied only `include/vulkan/`. FIX8 copies both `vulkan/` and sibling `vk_video/` directories.
