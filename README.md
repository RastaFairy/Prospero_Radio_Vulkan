# ProsperoRadio Vulkan para PlayStation 5

Bifurcación de ProsperoRadio que incorpora una ruta de representación Vulkan para PS5 y mantiene el sistema de compilación original. El código base se fija en la revisión `33898dd35375c1ae8370da137cfb6941d91c7684` de [blackbearreloaded/ProsperoRadio](https://github.com/blackbearreloaded/ProsperoRadio).

El lanzador de esta integración prepara una copia de trabajo Linux con esa revisión exacta, aplica los archivos de `overlay/` y compila el título. El `build.ps1` original se conserva sin cambios.

| Dato | Valor |
| --- | --- |
| Identificador del título | `PPSA99001` |
| Versión generada | `01.000.013` |
| API gráfica | Vulkan 1.0 |
| Lienzo lógico de la interfaz | 1920×1080 |

## Cambios de esta bifurcación

- RmlUi presenta la interfaz mediante un renderizador Vulkan nativo en vez de SDL Software Renderer. SDL sigue gestionando la entrada del mando y los temporizadores.
- El entorno de ejecución Vulkan para PS5 se enlaza estáticamente al título.
- El fondo usa una textura KTX2 RGBA8 de 3840×2160 con 12 niveles mip. La carga transmite los datos por filas mediante un búfer temporal de 2 MiB, sin crear en CPU otra copia completa de la imagen.
- Al arrancar, consulta VideoOut y selecciona un modo disponible. La interfaz conserva un lienzo lógico de 1920×1080 y se adapta al tamaño de presentación; no se fuerza una salida de 1080p ni se reduce la imagen maestra.
- La interfaz reserva dos conjuntos de búferes para fotogramas en vuelo: 2 MiB para vértices y 1 MiB para índices por conjunto. El conjunto de descriptores se limita a 256 conjuntos.
- La imagen del frontal de la radio forma el fondo. RmlUi presenta encima los datos dinámicos de la emisora y el estado de reproducción. La navegación sigue la lógica existente de la aplicación.

## Requisitos

- Windows 11 con WSL2 y la distribución Ubuntu 24.04, o Ubuntu 24.04 si se ejecuta el lanzador desde Linux.
- Git, Python 3, Make y las herramientas de compilación requeridas por ProsperoRadio y [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan).
- Conexión a Internet para obtener las dependencias públicas fijadas por los scripts.
- En Windows, guarda el proyecto en una ruta de unidad normal, por ejemplo `D:\prospero_modern`; no lo ejecutes desde una ruta de red `\\wsl$`.

## Compilación desde Windows

Abre PowerShell en la raíz del repositorio y ejecuta:

```powershell
.\build-vulkan.ps1 -Mode packages
```

El lanzador usa por defecto la distribución `Ubuntu-24.04`. También puedes indicar otra distribución WSL con `-Distro` si tienes configurada esa instalación.

## Compilación desde WSL o Linux

Desde la raíz del repositorio, ejecuta:

```bash
bash ./build-vulkan.sh packages
```

El lanzador detecta si el proyecto está en una unidad Windows montada y mueve la ejecución prolongada a la caché del sistema Linux, donde Git y las herramientas pueden trabajar con normalidad.

### Modos disponibles

- `packages`: compila la aplicación y genera los paquetes.
- `app`: compila y genera la carpeta de la aplicación.
- `check`: ejecuta el objetivo de comprobación del proyecto.
- `clean`: limpia los productos de compilación.

Los resultados aparecen en `out/`. El código base y las dependencias descargadas se guardan en `~/.cache/prospero-radio-modernized`, fuera del árbol de trabajo de Git. La primera compilación necesita conexión a Internet y puede tardar mientras se preparan las dependencias.

## Archivos añadidos para esta integración

El repositorio incluye los siguientes archivos propios de la ruta Vulkan:

```text
.gitignore
README.md
UPSTREAM-NOTICE.md
build-vulkan.ps1
build-vulkan.sh
overlay/PHYSICAL_RADIO_SKIN.md
overlay/VULKAN-INTEGRATION.md
overlay/apply-vulkan.py
overlay/check-runtime-texture-budget.py
overlay/setup-ps5-vulkan.sh
overlay/assets/ui/art/radio_front_4k.ktx2
overlay/assets/ui/art/radio_front_4k.tga
overlay/assets/ui/main.rml
overlay/assets/ui/styles/app.rcss
overlay/assets/ui/vulkan/ui.frag
overlay/assets/ui/vulkan/ui.vert
overlay/src/ps5_vulkan_renderer.cpp
overlay/src/ps5_vulkan_renderer.hpp
overlay/tools/build-mesa-util.sh
overlay/tools/check-vulkan-link-duplicates.py
overlay/tools/patch-ps5-vulkan-warning-policy.py
```

La imagen `.ktx2` es la textura que se carga en ejecución. La imagen `.tga` se conserva como fuente de validación del contenedor y de sus niveles mip. Los sombreadores GLSL se compilan a SPIR-V durante la preparación del título.

Los paquetes instalables, registros de consola, fotografías y cachés locales no se incluyen: se generan o conservan fuera del repositorio.

## Licencia y atribución

ProsperoRadio se distribuye bajo GPL-3.0-or-later. Consulta `LICENSE` y `UPSTREAM-NOTICE.md`. La integración usa el proyecto público [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan); revisa también los avisos de licencia de sus componentes antes de redistribuir una compilación.