# Créditos y agradecimientos

Prospero Radio se apoya en proyectos abiertos de la escena de PS5 y en
componentes de software libre. Esta página distingue el origen del proyecto,
las dependencias del build y los proyectos consultados como referencia. Un
agradecimiento o enlace no implica que el código de ese proyecto esté incluido
en Prospero Radio. Para consultar las fuentes agrupadas por su función en el
proyecto y en futuras correcciones, véase el [índice técnico](FUENTES-TECNICAS.md).

## Proyecto y base de trabajo

- [BlackBearReloaded](https://github.com/blackbearreloaded) — autor de
  [ProsperoRadio](https://github.com/blackbearreloaded/ProsperoRadio), proyecto
  original del que deriva este fork, y de
  [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
  base de la aplicación nativa y su flujo de construcción.
- [RastaFairy](https://github.com/RastaFairy) — mantenimiento de Prospero Radio
  Vulkan, diseño de la interfaz, integración de funciones y validación en PS5.
  Las capturas y observaciones de la versión 01.000.042 también fueron aportadas
  por RastaFairy.

## Desarrolladores y proyectos de la escena PS5

- [Mihawk-99 — PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan/tree/085aac6a9e42c0d6337660e7148eb8990604052a)
  — driver Vulkan para PS5 utilizado como base de la ruta gráfica de este
  proyecto; el build fija esa revisión.
- [John Törnblom y ps5-payload-dev](https://github.com/ps5-payload-dev) —
  [PS5 Payload SDK v0.42](https://github.com/ps5-payload-dev/sdk),
  [SDL para PS5](https://github.com/ps5-payload-dev/SDL/tree/release-2.30.x-ps5),
  [websrv](https://github.com/ps5-payload-dev/websrv) y
  [elfldr](https://github.com/ps5-payload-dev/elfldr). El SDK forma parte del
  toolchain; websrv y elfldr son proyectos de referencia y carga usados durante
  el trabajo con la consola.
- [BlackBearReloaded — ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl)
  — SDK y compilador de shaders PS5 OpenGL (v0.3.0) usado por el proceso de
  preparación de PS5_Vulkan.
- [Drakmor — ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) —
  herramienta del entorno de consola empleada para montar y probar paquetes.
- [itsPLK — PS5 Payload Manager](https://github.com/itsPLK/ps5-payload-manager)
  — referencia y herramienta para gestionar y cargar payloads.
- [tsuramatsu1 — APR Emu Updater](https://github.com/tsuramatsu1/apr-emu-updater)
  — referencia de una interfaz web que trabaja junto a un payload en la consola.
- [Gezine — BD-JB5](https://github.com/Gezine/BD-JB5) y
  [BenNoxXD — PS5-BDJ-HEN-loader](https://github.com/BenNoxXD/PS5-BDJ-HEN-loader)
  — referencias de la escena para el arranque y cierre controlado de procesos
  asociados al reproductor de discos.
- [ArkSama — PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon)
  — referencia de una API de cooperación entre una aplicación y un payload.

Estos proyectos se reconocen como trabajos consultados o herramientas del
entorno; Prospero Radio conserva su propio payload puente y su implementación.

## Componentes, especificaciones y herramientas

- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) — componentes y código común
  de gráficos utilizados por la implementación PS5_Vulkan.
- [Khronos Group — Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
  y [Vulkan-Docs](https://github.com/KhronosGroup/Vulkan-Docs) — cabeceras,
  registro y especificación de la API Vulkan.
- [LLVM](https://github.com/llvm/llvm-project) — Clang y lld para compilar y
  enlazar la aplicación.
- [SDL](https://github.com/libsdl-org/SDL) — biblioteca original de entrada y
  temporización; el port de PS5 está enlazado arriba.
- [RmlUi](https://github.com/mikke89/RmlUi) — interfaz de documentos, diseño y
  presentación.
- [FreeType](https://github.com/freetype/freetype) — biblioteca de fuentes
  incluida como dependencia estática de RmlUi.
- [SQLite](https://sqlite.org/) — almacenamiento local del catálogo.
- [zlib](https://github.com/madler/zlib) — compresión usada por las herramientas
  nativas del proyecto.
- [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) y
  [SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) — creación de los
  formatos de paquete FFPFSC y FFPKG.
- [ps5-payload-dev/PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) —
  distribución de bibliotecas PS5, incluido SQLite para el catálogo local.

Se conservan los avisos de copyright y las licencias de cada componente junto a
sus archivos o en sus respectivos repositorios. El código propio del fork se
distribuye bajo GPL-3.0-or-later; consulta [`LICENSE`](../LICENSE) y el
[resumen informativo en castellano](../LICENSE-ES.md). Para componentes de
terceros prevalece la licencia indicada por cada proyecto.
