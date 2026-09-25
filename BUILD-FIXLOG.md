# Corrección de build — 2.2.1 FIX1

## Error observado

`setup-ps5-vulkan.sh` fallaba inmediatamente con:

```text
cd: /home/user/.cache/prospero-radio-modernized/deps: No such file or directory
```

La causa era que el script intentaba resolver `CACHE_ROOT` mediante `cd` antes de crear ese directorio.

## Corrección

El script ahora:

1. toma la ruta de caché sin hacer `cd` prematuro;
2. ejecuta `mkdir -p` sobre la caché;
3. resuelve entonces la ruta absoluta;
4. ejecuta `make deps` de `PS5_Vulkan`;
5. adapta el PS5 OpenGL SDK 0.3.0 antes de ejecutar `fetch-mesa.sh` y `build-psbc-ps5.sh`.

## Estado de verificación

- `bash -n overlay/setup-ps5-vulkan.sh` — OK
- `python3 -m py_compile overlay/apply-vulkan.py` — OK
- No se afirma un `eboot.bin` ni un `.ffpkg` generado en este entorno.


FIX8: Vulkan header staging corrected. The PS5 driver now builds; title compilation failed because `.local/vulkan/include/vulkan_core.h` referenced missing `vk_video/*` headers after setup script copied only `include/vulkan/`. FIX8 copies both `vulkan/` and sibling `vk_video/` directories.
