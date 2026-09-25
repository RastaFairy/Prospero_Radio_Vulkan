# ProsperoRadio — Contexto persistente del proyecto

**Proyecto:** ProsperoRadio Modern / PS5 Homebrew
**Estado de trabajo:** FIX12 / enlace final en depuración
**Fecha de contexto:** 2026-09-25

## 1. Objetivo

El objetivo es reparar el código del ZIP modificado para llegar a una versión que pueda pasar a compilación real para PS5, preservando los cambios ya establecidos:

- sustituir el camino de renderizado SDL por Vulkan;
- mantener una interfaz moderna, de calidad homebrew y adaptada a PS5;
- conservar la funcionalidad radio existente;
- corregir integración, bootstrap, linker, rutas, stubs y dependencias;
- evitar reescribir o degradar la UI ya implementada;
- dejar un flujo de build reproducible.

La prioridad actual NO es añadir nuevas funciones. Es conseguir un árbol consistente y compilable.

## 2. Fuentes de referencia

### Upstream
Repositorio original:
https://github.com/blackbearreloaded/ProsperoRadio

Referencia de trabajo inspeccionada:
`33898dd35375c1ae8370da137cfb6941d91c7684`

### Vulkan
Repositorio de referencia del backend Vulkan PS5:
https://github.com/mihawk-99/PS5_Vulkan

Estado consultado:
commit `085aac6a9e42c0d6337660e7148eb8990604052a`

Repositorio de integración de ejemplo:
https://github.com/mihawk-99/PS5_vkQuake

Se verificó que este proyecto integra el driver Vulkan mediante enlaces estáticos, no mediante carga dinámica del `.so` en tiempo de ejecución.

## 3. Artefactos de trabajo

ZIP original recibido del usuario:
`/mnt/data/prospero_modern.zip`

ZIP reparado generado:
`/mnt/data/prospero_modern_FIXED5.zip`

SHA-256 conocido del FIX5:
`924430c9d2a32bda786e0c347b7960da3da76b091538451347e5ab40d3c38765`

Documentación/registro incluido en el proyecto:
- BUILD-FIX2.md
- BUILD-FIX3.md
- BUILD-FIX4.md
- BUILD-FIX5.md
- BUILD-FIXLOG.md
- BUILD-STATUS.md
- FIX4-REGRESSION.md
- README.md
- UPSTREAM-NOTICE.md

## 4. Arquitectura actual observada

El proyecto usa:

- SDL como capa base/infraestructura existente;
- RmlUi para la interfaz;
- una UI RML moderna;
- radio_service para catálogo/reproducción;
- radio_input para DualSense;
- radio_ime para entrada de texto;
- fonts bitmap/RmlUi para la presentación;
- toolchain Prospero/PS5;
- runtime `libc.prx` generado por el propio bootstrap;
- driver Vulkan PS5 integrado estáticamente.

IMPORTANTE: SDL no debe continuar siendo el renderer gráfico final. Puede permanecer como dependencia/capa auxiliar únicamente cuando sea estrictamente necesaria para compatibilidad, pero el camino de presentación debe ser Vulkan.

## 5. Hallazgos críticos ya identificados

### 5.1 Build del proyecto modificado
El árbol era un overlay/bootstrap que reconstruye el proyecto contra el commit upstream fijado. Por tanto, los parches deben existir en el overlay y no depender de modificaciones manuales fuera del mismo.

### 5.2 Integración Vulkan
El driver PS5_Vulkan se entrega como librerías estáticas. El ejemplo PS5_vkQuake confirma los artefactos esenciales:

- `build/driver/ps5/libps5vk.ps5.a`
- `.deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a`
- `build/driver/ps5/libpsbc_driver.ps5.a`
- `.deps/native/psbc/lib/libpsbc_support.ps5.a`

El título debe enlazarlos estáticamente.

### 5.3 Objetos Mesa adicionales
El driver deja fuera del archive estático tres fuentes que el ejecutable necesita cuando no existe el margen de símbolos indefinidos de un `.so`:

- `src/util/u_thread.c`
- `src/util/anon_file.c`
- `src/util/os_file.c`

PS5_vkQuake los compila mediante `tools/build-mesa-util.sh`. ProsperoRadio FIX5 adopta la misma estrategia conceptual.

### 5.4 Problema de rutas
El build original validaba rutas de archivos con patrones relativos al root. Pasar rutas absolutas desde variables como `APP_VULKAN_ARCHIVES` provocaba rechazo del propio validador.

La integración corregida usa rutas relativas cuando corresponda.

### 5.5 Runtime C++
El driver Vulkan y partes de Mesa requieren:

- `libc++.a`
- `libc++abi.a`
- `libunwind.a`
- builtins de Clang cuando están disponibles

El enlace debe mantener el grupo de dependencias.

### 5.6 SDL renderer residual
`src/main.cpp` upstream todavía contenía `SdlRenderInterface`, `SDL_CreateSoftwareRenderer`, `SDL_RenderGeometry`, `SDL_RenderCopy`, `SDL_UpdateWindowSurface`, etc.

El objetivo FIX5 es eliminar el camino software SDL del bucle de render final y no dejar un segundo renderer activo de manera accidental.

### 5.7 Vulkan WSI
Cuando la aplicación crea superficie mediante funciones Vulkan debe habilitarse la extensión apropiada, incluyendo `VK_KHR_surface` y el backend de display utilizado.

### 5.8 Shader compiler
Las rutas del shader compiler deben resolverse desde la raíz real del proyecto/SDK y no desde un checkout sibling que pueda no existir.

## 6. Cambios que NO deben revertirse

Preservar:

- interfaz moderna actual;
- RmlUi;
- tarjetas de emisoras;
- vistas Popular / Trending / Voted / Favorites / Discover;
- búsqueda y filtros;
- overlays de búsqueda/créditos;
- navegación DualSense;
- radio service existente;
- lógica de reproducción/parada;
- soporte de fuentes bitmap/multilingüe;
- assets y diseño visual actuales;
- mejoras ya documentadas en BUILD-FIX*.md;
- futuras correcciones de concurrencia/backups de la línea de trabajo anterior cuando formen parte del árbol real.

## 7. Invariantes funcionales

No cambiar sin motivo:

- estados COPIAR / RESTORE / PAPELERA;
- `backups-changed` cuando represente únicamente seguridad/motor;
- `ps5-trash-changed` y `pc-trash-changed` como eventos independientes;
- secuencia de operaciones destructivas;
- cancelación antes de `Dispose` en tareas concurrentes;
- deduplicación de descargas;
- actualización de cada resultado inmediatamente cuando corresponda.

## 8. Relación con la línea de trabajo anterior

Existe una línea previa de correcciones centrada en versión `v6.8.7.26`, con especial atención a:

- backups;
- PC y papelera;
- `WarmAsync` global;
- tareas fire-and-forget;
- backups secuenciales;
- mostrar cada resultado inmediatamente;
- deduplicación;
- paralelismo controlado;
- cancelación antes de Dispose;
- consumidores unificados sin alterar estados COPIAR / RESTORE / PAPELERA.

Estas reglas son contexto funcional y deben respetarse si esos módulos forman parte del ZIP final.

## 9. Build esperado

Entorno previsto:
- Linux / WSL;
- Bash;
- Make;
- Python 3;
- Clang/LLD;
- SDK PS5 descargado por el bootstrap;
- dependencias Vulkan PS5 preparadas.

Flujo general esperado:

1. `make doctor`
2. `make deps`
3. preparar PS5_Vulkan sibling o indicar `PS5_VULKAN_DIR`
4. construir dependencias Vulkan:
   - `fetch-mesa.sh`
   - `build-psbc-ps5.sh`
   - `build-vulkan-runtime.sh`
   - `build-driver.sh`
5. ejecutar build del título
6. verificar `dist/<TITLE_ID>/eboot.bin`
7. inspeccionar/firmar/ensamblar
8. opcionalmente empaquetar/deployar

## 10. Estrategia de validación

Antes de considerar el árbol final:

### Estructura
- el overlay reconstruye exactamente el commit esperado;
- todos los archivos modificados están presentes;
- no hay referencias a paths locales personales;
- no hay archivos generados corruptos.

### Código
- `main.cpp` no deja un renderer SDL software activo;
- includes Vulkan y headers del SDK son coherentes;
- llamadas Vulkan usan extensiones declaradas;
- símbolos del driver tienen proveedor válido;
- no hay doble definición de runtime/stubs.

### Build
- `bash -n` sobre scripts;
- parseo JSON;
- lint del proyecto;
- compilación host de herramientas;
- build PS5 real en el entorno del usuario;
- enlace sin unresolved symbols;
- generación correcta de `eboot.bin`;
- inspección de SELF.

### Runtime
Solo después de compilar:
- arrancar homebrew;
- comprobar splash/entrada;
- comprobar render Vulkan;
- comprobar navegación DualSense;
- comprobar catálogo;
- reproducir emisora;
- comprobar overlays y búsqueda;
- comprobar cierre limpio.

## 11. Estado exacto al cerrar este bloque

El proyecto FIX5 está preparado conceptualmente para pasar a la etapa de compilación real.

No se debe afirmar que existe una compilación PS5 exitosa hasta que el usuario ejecute el toolchain real o se disponga de ese toolchain en el entorno.

La tarea inmediata siguiente es:

> ejecutar el build real del ZIP FIX5 en WSL/Linux y corregir iterativamente los errores de compilación/enlace que aparezcan, sin revertir la arquitectura Vulkan/UI.

## 12. Regla de continuidad para futuras sesiones

Cuando el usuario diga "continuar ProsperoRadio", cargar este documento como contexto base.

Proceder en este orden:
1. identificar el ZIP/árbol actual;
2. comprobar la revisión base;
3. leer `BUILD-STATUS.md`, `BUILD-FIXLOG.md`, `BUILD-FIX5.md`;
4. verificar los cambios Vulkan/UI actuales;
5. ejecutar o analizar el siguiente error de build;
6. modificar solo lo necesario;
7. registrar la corrección en un nuevo `BUILD-FIXN.md`;
8. regenerar el ZIP de entrega;
9. actualizar este contexto persistente.

## 13. Decisiones de diseño ya tomadas

- Vulkan es el renderer objetivo.
- El driver se enlaza estáticamente; no depender de `dlopen` de un `.so` del repositorio.
- La UI moderna existente se conserva.
- El build debe ser reproducible y documentado.
- Los fixes deben ser incrementales y trazables.
- Nunca declarar "compila" sin una verificación real del toolchain.

## FIX9 — PS5 Vulkan renderer API compatibility

The first ProsperoRadio C++ compile after FIX8 reached `src/ps5_vulkan_renderer.cpp` and failed on three Vulkan API mismatches:
- `VkDisplayPlanePropertiesKHR::currentStack` was corrected to `currentStackIndex`.
- `VkDisplayPlaneCapabilitiesKHR` does not contain `currentTransform`; the renderer now selects a supported `VkSurfaceTransformFlagBitsKHR` from `VkDisplayPropertiesKHR::supportedTransforms` and uses that transform for `VkDisplaySurfaceCreateInfoKHR`.
- `vkCmdEndRenderPass` returns `void` and is now called directly rather than passed to `CheckResult`.

FIX9 artifact: `/mnt/data/prospero_modern_FIXED9.zip`
SHA-256: 361ab6dc6d4776bf310c1f61bb207a964bd8ac157dee3963700672fffeb8a177

Previous milestones remain intact: FIX6 Mesa generated-source bootstrap, FIX7 staged `ps5-native-tool`, FIX8 Vulkan headers including `vk_video`, static PS5_Vulkan driver/runtime integration, and modern RmlUi/Vulkan UI architecture.

## FIX9 / FIX10 status — 2026-09-25

- FIX9 reached full compilation of `src/ps5_vulkan_renderer.cpp`; the earlier C++ errors (`currentStack`, `currentTransform`, and `vkCmdEndRenderPass` return handling) were repaired.
- The next failure occurred at final title link: duplicate symbols from both `.local/vulkan/lib/libpsbc.ps5.a` and `.local/vulkan/lib/libpsbc_driver.ps5.a` (`psbc_init`, `psbc_shutdown`, `psbc_compile_shader`, `crc32_sb`, `ac_get_harvested_configs`, etc.).
- Root cause: `PS5_Vulkan/tools/build-driver.sh` already derives `libpsbc_driver.ps5.a` from the original `libpsbc.ps5.a` using `prospero-objcopy`, renaming five runtime symbols (`vk_debug_report`, `vk_format_get_ycbcr_info`, `vk_format_to_pipe_format`, `vk_sampler_state_init`, `vk_spec_info_to_nir_spirv`). The title must not link the original PSBC archive a second time.
- FIX10 changes `overlay/setup-ps5-vulkan.sh` archive staging to exclude only `libpsbc.ps5.a`. It keeps `libpsbc_driver.ps5.a`, `libpsbc_support.ps5.a`, `libvk_runtime.ps5.a`, and `libps5vk.ps5.a`.
- FIX10 ZIP: `/mnt/data/prospero_modern_FIXED10.zip`
- FIX10 SHA-256: `39cbb6f0fa88893d0e6618faffe5433a0dc8ee505b1a7c2802eba19fd53bb117`
- Validation performed: `bash -n overlay/setup-ps5-vulkan.sh`; archive filter explicitly verifies `! -name 'libpsbc.ps5.a'`.
- Next expected stage: final title link. Do not claim PS5 build success until `build.ps1` reaches eboot/package creation and validation.

## FIX11 status — 2026-09-25

The FIX10 build reached complete compilation of `src/ps5_vulkan_renderer.cpp` and then failed at final title link because the PS5_Vulkan support archive and the title each defined `__assert` and `localtime_r`.

Verified sources:
- `src/main.cpp` had a title-local `extern "C" void __assert(...)` fallback.
- `src/sqlite_compat.cpp` had a title-local `extern "C" struct tm *localtime_r(...)` stub returning `nullptr`.
- `PS5_Vulkan/tooling/psbc/psbc_ps5_shims.c` intentionally provides both symbols for the Mesa/Vulkan runtime and is packaged in `libpsbc_support.ps5.a`.

FIX11 changes:
- remove those two obsolete title-local definitions in the overlay after materializing the pinned upstream source;
- retain the PS5_Vulkan shim implementations;
- add `overlay/tools/check-vulkan-link-duplicates.py` and run it immediately before the final title `prospero-lld` invocation against title objects, extra Mesa objects, and the Vulkan whole-archive inputs.

This is the first explicit pre-link symbol-ownership audit in the project. Future duplicate-symbol failures in the same link domain should be diagnosed before invoking `ld.lld`.

Artifact:
- `prospero_modern_FIXED11.zip`

Next expected stage: the pre-link symbol audit should either report a clean link input set or name the next duplicate owner pair before `prospero-lld` runs.


## 11. Estado actualizado FIX12

La ejecución real ya ha superado las dependencias principales de Vulkan y ha llegado a compilar `src/ps5_vulkan_renderer.cpp`. Las correcciones aplicadas desde FIX5 fueron, en orden de diagnóstico: generación de fuentes Mesa faltantes (`u_format_gen.h` y relacionadas); staging de `ps5-native-tool`; staging completo de `vk_video` junto a `vulkan`; adaptación del renderer a los nombres/retornos de Vulkan-Headers; eliminación del `libpsbc.ps5.a` original del enlace del título para evitar duplicarlo con `libpsbc_driver.ps5.a`; eliminación de los fallbacks locales `__assert` y `localtime_r`; y finalmente una auditoría previa de símbolos fuertes.

FIX11 no llegó a compilar porque su propio bootstrap buscaba el comando `prospero-lld` mediante una cadena frágil que contenía `\\n` literal y además instalaba la auditoría antes de crear las variables `vulkan_archives` y `extra_objects`. FIX12 reemplaza esa estrategia por una búsqueda estructural y cambia el orden de instalación del parche.

Se han realizado pruebas sintéticas de `patch_target_build_driver()` + `patch_link_duplicate_audit()`, `bash -n` sobre el `tools/build.sh` generado y una prueba real con `nm` que verifica detección de símbolos ELF duplicados.

No se debe considerar el proyecto compilado hasta obtener de la ejecución real `eboot.bin` y la validación de los paquetes.


## FIX13 status — 2026-09-25

La ejecución real posterior a FIX12 superó la compilación del renderer Vulkan y llegó a `packages`, pero falló antes del enlace final porque `tools/check-vulkan-link-duplicates.py` no estaba presente en el worktree materializado. El script sí estaba dentro del overlay del ZIP, pero `overlay/apply-vulkan.py` no lo copiaba a `tools/`.

FIX13 añade `stage_overlay_tools()`, copia explícitamente `build-mesa-util.sh` y `check-vulkan-link-duplicates.py`, comprueba que el auditor haya quedado materializado y mantiene el orden `patch_target_build_driver()` -> `patch_link_duplicate_audit()`. La prueba sintética de staging + parcheado y `bash -n` del `tools/build.sh` generado han pasado.

El siguiente objetivo sigue siendo la ejecución real del enlace y la generación de `eboot.bin`; no se debe declarar éxito PS5 antes de esa evidencia.
