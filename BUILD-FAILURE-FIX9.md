# ProsperoRadio Modern — BUILD FIX9

## Failure addressed

The FIX8 build reached the ProsperoRadio C++ compilation and failed in `src/ps5_vulkan_renderer.cpp` with three Vulkan API mismatches:

1. `VkDisplayPlanePropertiesKHR::currentStack` was replaced by the Vulkan-defined `currentStackIndex`.
2. `VkDisplayPlaneCapabilitiesKHR` has no `currentTransform`. FIX9 selects a valid surface transform from `VkDisplayPropertiesKHR::supportedTransforms` and stores it in `surface_transform_`.
3. `vkCmdEndRenderPass` returns `void`, so it is called directly rather than passed to `CheckResult(VkResult, ...)`.

The existing VK_KHR_display path, static PS5_Vulkan driver integration, and RmlUi/Vulkan architecture are preserved.
