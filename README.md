# ProsperoRadio Vulkan Edition 2.2.1

Paquete de build reproducible para `blackbearreloaded/ProsperoRadio` con la remodelación de interfaz física de radio y un renderer Vulkan para PS5 basado en `mihawk-99/PS5_Vulkan`.

## Base real del proyecto

- Proyecto original: `https://github.com/blackbearreloaded/ProsperoRadio`
- Commit de base: `33898dd35375c1ae8370da137cfb6941d91c7684`
- Title ID: `PPSA99001` (se conserva)
- Versión de esta integración: `01.000.015`
- Driver Vulkan: `https://github.com/mihawk-99/PS5_Vulkan`

El ZIP que aportaste sigue siendo el origen de la remodelación de interfaz. El código funcional de radio, input, empaquetado y la infraestructura nativa de PS5 se materializa desde el repositorio original fijado arriba.

## Arquitectura gráfica

```text
ProsperoRadio
    |
    +-- SDL2 ----------------------> input / reloj / servicios
    |
    +-- RmlUi
          |
          +-- ProsperoVulkanRenderInterfaceAdapter
                  |
                  +-- Ps5VulkanRenderInterface
                          |
                          +-- Vulkan 1.0
                                  |
                                  +-- PS5_Vulkan / Mesa runtime
                                  +-- ps5vk / PS5BC
                                  +-- AGC
                                  +-- VideoOut
                                  |
                                  +--> GPU PS5
```

Se ha eliminado `SDL_CreateSoftwareRenderer()` del camino de dibujado de RmlUi. La presentación pasa por Vulkan/PS5_Vulkan. SDL permanece porque la aplicación ya depende de él para input y temporización.

`PS5_Vulkan` documenta además que un título PS5 no puede cargar dinámicamente un módulo gráfico construido en el repositorio mediante `dlopen`, por lo que esta integración prepara los archivos estáticos del driver para enlazarlos en `eboot.bin`.

## Renderer incluido

El nuevo renderer implementa la interfaz de compatibilidad de RmlUi que utilizaba el ProsperoRadio original e incluye:

- `VK_KHR_display` para la superficie/presentación de PS5.
- `VK_KHR_swapchain`.
- render pass, framebuffer y pipeline Vulkan.
- geometría RmlUi texturizada y no texturizada.
- blending alfa.
- scissor regions.
- texturas TGA 32-bit y atlas RTA1 del camino de textura original.
- texturas generadas por RmlUi.
- coordenadas de UI de 1920×1080 escaladas al modo de salida seleccionado.
- shaders GLSL 450 que se compilan a SPIR-V durante el build.

## Interfaz remodelada

Se conservan los cambios del paquete 2.2.1:

- cuadrícula 3×2 de emisoras;
- navegación de seis tarjetas;
- skin de radio física;
- fondo maestro 3840×2160 en KTX2 RGBA8 con mipmaps, leído y enviado a Vulkan por bloques para evitar una copia completa de 4K en memoria de CPU;
- salida Vulkan ajustada al modo activo de VideoOut (incluye 4K y 1440p), manteniendo el lienzo lógico de la interfaz y escalándolo al tamaño nativo de salida;
- RmlUi minimalista compatible con el renderer;
- fuentes bitmap y tamaños ya validados por el proyecto.

## Build en PS5

La compilación real se verificó el 25 de septiembre de 2026 con `build.ps1` en WSL2 Ubuntu 24.04. Generó `out/PPSA99001/eboot.bin`, `out/PPSA99001.ffpkg` y `out/PPSA99001.ffpfsc`. El ejecutable firmado pasó la validación de integridad; el FFPKG se creó como imagen UFS2 válida y el FFPFSC pasó su comprobación posterior sin warnings ni errores.

## Fallo de inicio y versión 01.000.016

El klog del 25-09-2026 registró 35 crashes `SIGILL` (`rip=0x4001a9`) en las
versiones 01.000.010→015: el runtime de asignación del boilerplate
(`app_cpp_runtime.cpp`) ejecutaba `__builtin_trap()` cuando `malloc` devolvía
NULL, y el `stderr` del título se descartaba en consola, así que el fallo no
dejaba información. La cadena simbolizada con `build/llvm-pie.elf` muestra el
crash dentro de la carga de texturas de la UI (`ReadWholeFile →
vector::resize → operator new`); el "malloc de 34 GB" de informes anteriores era
en realidad un puntero interno de libkernel (`SceKernelInternalMemory`), no un
tamaño. El diagnóstico completo está en `CRASH-DIAGNOSIS-2026-09-25.md`.

La versión 01.000.016 (`BUILD-FIX27.md`) corrige el mecanismo de fallo:

- el runtime redirige `stderr` a `/download0/prospero-radio.log` (descargable
  por el FTP de etaHEN), por lo que todos los diagnósticos del renderer son ya
  visibles;
- un fallo de asignación registra el tamaño pedido y un rastro de las últimas
  32 asignaciones antes de `abort()`;
- los pedidos superiores a 1 GiB se rechazan de forma controlada.

Las versiones 01.000.014/015 (límites de lectura de 16 MiB, validación de
recuentos Vulkan, controles de stick/triángulo, stick izquierdo volumen, stick
derecho sintonizar, cruceta listas, triángulo ajustes) se conservan. Las builds
anteriores están preservadas en `recovery/`.

La compilación y el empaquetado de 01.000.015 terminaron con `build.ps1 -Mode packages` en WSL2 Ubuntu 24.04. `eboot.bin` pasó la validación de integridad de firma; el FFPKG se verificó como imagen UFS2 y el FFPFSC pasó su comprobación posterior con cero warnings y errores. SHA-256 del resultado:

- `eboot.bin`: `B003ADEEC00377686DF8B1D724E64278B656DB87359D048145569677DB814223`
- `PPSA99001.ffpkg`: `423CB8C8DA3D2B0D7688E2AA8F556DAB178164B088CA63ED5F36B0E6574BC532`
- `PPSA99001.ffpfsc`: `453305F96DFA8ECC008B52DF10207B5F8BAC7CB0507D6C84ED3EDDF590A672A9`

La ejecución en PS5 todavía requiere instalar y lanzar 01.000.015; el klog disponible corresponde a 01.000.013, así que el comportamiento en hardware de esta build permanece por validar. La versión 01.000.014 anterior está preservada en `recovery/pre-01.000.015/` con su manifiesto SHA-256.

El paquete sí contiene todo lo necesario para que el entorno de build del proyecto haga el trabajo real:

1. materializa el commit exacto de ProsperoRadio;
2. aplica la UI 2.2.1;
3. sustituye la ruta gráfica SDL Software por el renderer Vulkan;
4. crea la caché de dependencias de `PS5_Vulkan` correctamente antes de entrar en ella;
5. ejecuta `make deps` de `PS5_Vulkan`;
6. materializa las fuentes `third_party` del **PS5 OpenGL SDK 0.3.0** con `tools/fetch-sources.py`;
7. compila/verifica PSBC con `tools/build-psbc-ps5.sh`;
8. adapta el **PS5 OpenGL SDK 0.3.0** con `tools/adapt-opengl-sdk.sh`;
9. prepara Mesa y continúa con el runtime Vulkan y el driver `ps5vk`;
10. copia headers, librerías estáticas e import stubs de PS5 Vulkan;
11. compila los shaders SPIR-V;
12. enlaza las librerías Vulkan en el título;
13. ejecuta el `make packages` original y devuelve el resultado a `out/`.

La versión actual de `PS5_Vulkan` no incluye ese SDK dentro de su repositorio. FIX4 mantiene un bootstrap reproducible: si no se proporciona `PS5_OPENGL_SDK` o `PS5_OPENGL_RELEASE`, clona automáticamente `https://github.com/blackbearreloaded/ps5-opengl.git` fijado a `v0.3.0` dentro de la caché. Antes de compilar/verificar PSBC, ejecuta `python3 tools/fetch-sources.py` sobre ese checkout para materializar `third_party/opengnm-psbc`, y después pasa ese mismo checkout a `tools/adapt-opengl-sdk.sh`. La documentación actual del proyecto confirma que este paso de adaptación es necesario antes de `fetch-mesa` y `build-psbc-ps5.sh`. citeturn949128view0turn772388view1

## Windows 11 + WSL2

Desde PowerShell:

```powershell
.\\build.ps1
```

Construcción directa desde WSL:

```bash
bash ./build.sh packages
```

La integración usa la misma estructura de WSL2 del paquete 2.2.1 y mantiene el checkout nativo en el filesystem Linux.

## Variables útiles

Se puede fijar una revisión concreta de `PS5_Vulkan` sin modificar los archivos del paquete:

```bash
export PS5_VULKAN_REF=<commit-o-tag>
```

Por defecto se usa `main` del repositorio `mihawk-99/PS5_Vulkan`.

## Integridad

El script `overlay/apply-vulkan.py` se niega a trabajar sobre otra revisión de ProsperoRadio. Esto evita aplicar las modificaciones sobre un `main` posterior cuyo contrato de renderer o de RmlUi haya cambiado.
