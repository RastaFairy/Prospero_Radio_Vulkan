# ProsperoRadio Vulkan FIX3

El bootstrap de PS5_Vulkan se corrigió para respetar el orden requerido por el upstream actual:

1. `make deps`
2. resolver/clonar `ps5-opengl` v0.3.0
3. `PS5_OPENGL_SDK="$OPENGL_INPUT" bash tools/build-psbc-ps5.sh`
4. `PS5_OPENGL_SDK="$OPENGL_INPUT" bash tools/adapt-opengl-sdk.sh "$OPENGL_INPUT"`
5. `tools/fetch-mesa.sh`
6. `tools/build-vulkan-runtime.sh`
7. `tools/build-driver.sh`

`adapt-opengl-sdk.sh` exige explícitamente que exista `.deps/work/psbc-ps5`, que crea `build-psbc-ps5.sh`. El error anterior era, por tanto, un orden de bootstrap incorrecto, no una ausencia adicional del SDK.
