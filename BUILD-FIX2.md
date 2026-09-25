# FIX2 — PS5 OpenGL SDK bootstrap

El build de FIX1 alcanzaba correctamente `PS5_Vulkan` y `make deps`, pero se detenía porque no existía `ps5-opengl-sdk-0.3.0` en la máquina.

FIX2 resuelve esa dependencia sin pedir una instalación manual:

1. Respeta `PS5_OPENGL_SDK` si el usuario ya tiene un checkout.
2. Respeta `PS5_OPENGL_RELEASE` si el usuario usa el bundle de release.
3. Reutiliza `$PROSPERO_BUILD_ROOT/deps/ps5-opengl-sdk-0.3.0` si ya existe.
4. Si no existe, clona `blackbearreloaded/ps5-opengl` fijado a `v0.3.0` en esa caché.
5. Comprueba `dependencies.json` y `toolchain/opengnm-psbc-host.mak` antes de ejecutar `tools/adapt-opengl-sdk.sh`.

Referencia upstream: el propio `PS5_Vulkan` documenta que el release 0.3.0 es el companion SDK y que `adapt-opengl-sdk.sh` acepta un checkout del repositorio. El release `v0.3.0` de `ps5-opengl` corresponde al commit `6cb291a`.
