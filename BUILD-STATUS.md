# ProsperoRadio Vulkan FIX4

FIX4 añade el fetch real de las fuentes fijadas de PS5 OpenGL antes de verificar/compilar PSBC.

Orden efectivo del bootstrap:

1. `make deps`
2. resolver/clonar `ps5-opengl` v0.3.0
3. `python3 tools/fetch-sources.py` sobre ese checkout para materializar `third_party/opengnm-psbc`
4. `PS5_OPENGL_SDK="$OPENGL_INPUT" bash tools/build-psbc-ps5.sh`
5. `PS5_OPENGL_SDK="$OPENGL_INPUT" bash tools/adapt-opengl-sdk.sh "$OPENGL_INPUT"`
6. `tools/fetch-mesa.sh`
7. `tools/build-vulkan-runtime.sh`
8. `tools/build-driver.sh`

`build-psbc-ps5.sh` solo verifica el árbol PSBC mediante `--verify-psbc`; FIX4 deja materializadas antes las fuentes `third_party` que esa verificación espera.
Esto corrige exactamente el `fatal: cannot change to .../third_party/opengnm-psbc` observado durante el build de FIX3.


FIX8: Vulkan header staging corrected. The PS5 driver now builds; title compilation failed because `.local/vulkan/include/vulkan_core.h` referenced missing `vk_video/*` headers after setup script copied only `include/vulkan/`. FIX8 copies both `vulkan/` and sibling `vk_video/` directories.


## FIX9
Corrected Vulkan API mismatches in `ps5_vulkan_renderer.cpp`: `currentStackIndex`, valid display surface transform selection, and `vkCmdEndRenderPass` void return.
