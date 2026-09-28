# Contribuir a Prospero Radio

Gracias por contribuir. Este proyecto integra Vulkan y RmlUi en una aplicación de radio para PS5. El build aplica los cambios de `overlay/` al árbol upstream fijado. Antes de modificarlo, consulta [`README.md`](README.md), [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/TESTING.md`](docs/TESTING.md) y [`AGENTS.md`](AGENTS.md).

## Antes de proponer un cambio

1. Busca una incidencia existente o abre una nueva con la versión probada, pasos reproducibles, resultado esperado y observado, y el entorno relevante. Para fallos de consola, indica firmware, método de carga y hash del artefacto si están disponibles.
2. Mantén el cambio acotado y edita la fuente correspondiente. Los cambios propios de la integración Vulkan, la interfaz y sus recursos suelen pertenecer a `overlay/`. No edites `out/`, `build/` ni `dist/` como si fueran fuentes: contienen resultados locales generados.
3. En Linux o WSL, sigue [`docs/TESTING.md`](docs/TESTING.md). En Windows con WSL2 y Ubuntu 24.04 puedes ejecutar `.\build-vulkan.ps1 -Mode check`; para verificar también el paquete, usa `.\build-vulkan.ps1 -Mode ffpfsc`.
4. Si cambias comportamiento, añade una comprobación de regresión enfocada cuando sea viable. Para cambios de RML/RCSS, fuentes bitmap, reproducción, codecs, consultas del catálogo o navegación del mando, registra las comprobaciones de host pertinentes y una prueba breve en PS5 antes de afirmar que el cambio está validado para release.
5. En el pull request, resume el motivo y el alcance, enumera las comprobaciones ejecutadas y sus resultados, y deja claro qué queda sin probar. Una compilación o prueba de host no demuestra por sí sola el funcionamiento en hardware; RmlUi/SDL, entrada física, audio nativo, carga del título y composición visual requieren evidencia en consola.

## Convenciones del repositorio

- No subas `.env`, credenciales, claves, dumps de consola, paquetes de juego, ejecutables del SDK, descargas locales de dependencias ni resultados locales de `out/`, `build/` o `dist/`.
- Conserva los avisos de copyright y `SPDX-License-Identifier: GPL-3.0-or-later` en los archivos propios que admitan comentarios. No elimines los avisos ni las licencias de dependencias vendorizadas.
- Usa `.hpp` para las interfaces C++ propias y C++20 para las unidades C++ propias. Mantén las dependencias vendorizadas en su forma upstream y expón lo necesario mediante adaptadores pequeños.
- Mantén deterministas las comprobaciones de host. No afirmes que una API o función específica de PS5 está verificada sin resultados obtenidos en la consola.
- Al modificar audio, conserva la gestión acotada de memoria, la cancelación y el tratamiento de streams malformados.
- Los tags de release deben coincidir exactamente con `contentVersion` de `sce_sys/param.json`, con formato `NN.NNN.NNN` y sin prefijo `v`.

El código propio se distribuye bajo GPL-3.0-or-later. Consulta [`LICENSE`](LICENSE) y su [resumen informativo en castellano](LICENSE-ES.md); el resumen no sustituye al texto de la licencia.
