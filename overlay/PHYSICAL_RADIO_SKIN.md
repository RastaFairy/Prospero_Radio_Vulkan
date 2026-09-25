# ProsperoRadio physical-radio skin

This iteration preserves the full 4K artwork and adapts Vulkan presentation to the PS5's active VideoOut mode.

## Texture and output path

- `assets/ui/art/radio_front_4k.tga` remains the 3840x2160 master image.
- `assets/ui/art/radio_front_4k.ktx2` is the runtime RGBA8 Vulkan texture. It contains all 12 mip levels at full source resolution.
- KTX2 level payloads use `VK_FORMAT_R8G8B8A8_UNORM`, which the bundled PS5 Vulkan driver supports for sampled images. The runtime does not request GPU-compressed BC formats because this driver reports its compressed texture formats as unsupported.
- The renderer reads the KTX2 header and mip index, then streams image rows through a 2 MiB Vulkan staging buffer. It never builds a full decoded CPU copy of the 4K texture.
- The fragment shader uses implicit texture LOD. Linear mip sampling therefore follows the actual pixel footprint as Vulkan renders to the selected output extent.
- At startup, the renderer reads the active resolution from VideoOut and chooses the matching Vulkan display mode, including 2560x1440 when that mode is active. It falls back to the largest available display mode if the query cannot be read.
- RmlUi retains a 1920x1080 logical viewport. Geometry and scissor regions scale to the selected Vulkan surface dimensions.

## Composition

- The backplate fills the complete logical surface.
- The old dashboard header, cards, status pill, pagination rail and footer are prevented from painting on the home screen.
- The existing `detail-panel` IDs render as plain text directly over the physical receiver's glass display.
- The six station cards remain in the DOM as zero-size/off-canvas logical focus nodes so existing C++ updates continue to work without visible rectangles.
- Physical lower keys are transparent RmlUi hit targets; the artwork supplies their labels and geometry.
- The old `now-panel` remains logically present but has no visible footprint; `RefreshPlayback` still updates its IDs.

## Memory behavior

The KTX2 mip payload totals about 42.2 MiB in GPU memory. Runtime CPU staging is bounded at 2 MiB; parsing metadata uses only a small mip index. Two in-flight frames reserve 2 MiB for vertices and 1 MiB for indices each, and geometry is converted directly into those mapped buffers without per-draw CPU vectors. The descriptor pool is capped at 256 sets, ample for the interface while avoiding the former 4096-set over-reservation. The full-resolution texture and mip chain remain intact. The startup checker verifies the asset dimensions, Vulkan format, all mip sizes, and container bounds before the native build begins.

The app package is still materialized from the pinned ProsperoRadio source by `build.sh`; upstream runtime files are not duplicated in this overlay.
