# Registro de cambios — ProsperoRadio Vulkan Edition

> **Versión actual del título: 01.000.017** · compilada y **ejecutable en consola PS5**.
>
> No confundir: *2.2.1* es la versión del paquete de interfaz original sobre el que se
> construye este fork; *01.000.0XX* es la `contentVersion` que ve la consola en
> `sce_sys/param.json` (la fija `overlay/apply-vulkan.py` en cada build).

---

## Por qué existe este fork

El [ProsperoRadio original](https://github.com/blackbearreloaded/ProsperoRadio) es una
radio para PS5 cuya interfaz se dibuja con el renderer software de SDL. Este fork
existe por tres motivos:

1. **Render por Vulkan.** Sustituye el camino de dibujado SDL software por
   [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (Mesa → AGC), manteniendo SDL
   solo para mandos y temporización: interfaz física de radio 3×2, fondo 4K en KTX2
   leído por bloques, salida ajustada al modo activo de VideoOut (incluye 4K).
2. **Memoria real de consola.** En el hardware, el heap de libc del título no crece más
   allá de unos pocos MB: cualquier asignación grande moría (35 crashes
   `SIGILL` en las builds 010→015, luego identificados al detalle). Este fork añade un
   **pool propio de Direct Memory de 512 MB** para las asignaciones grandes, con el
   heap de libc reservado para las pequeñas.
3. **Diagnóstico en hardware real.** Todo un historial de forense de crashes: log del
   runtime en `/download0/prospero-radio.log`, rastro de las últimas 32 asignaciones,
   backtraces simbolizados y el tooling en `out/` para analizar coredumps.

El detalle técnico por build está en `BUILD-FIX1..28.md`; este fichero es el resumen
para humanos.

---

## 01.000.017 — 2026-09-26 · ✅ ejecutable en consola (validado por el autor)

**La primera versión que arranca y funciona.**

- **Pool de Direct Memory de 512 MB**: el enlace intercepta
  `malloc/calloc/realloc/free/posix_memalign` y las asignaciones de 256 KB o más se
  sirven de mapeos Direct-Memory propios (con caché de reúso y spinlock); las pequeñas
  siguen en el heap de libc. Esto elimina la causa de todos los crashes anteriores.
- El runtime vive en `overlay/src/app_cpp_runtime.cpp` y se inyecta desde el build.
- Validado con test unitario en host (mocks del kernel + estrés multihilo).

**Pendiente / conocido:**

- Descuadres y alineaciones de texto en la interfaz (reportado; probablemente ajuste de
  escalado del lienzo lógico 1920×1080 contra el modo de salida real).
- Validar cambios de modo de vídeo (4K / 1440p / 1080p) y frecuencia.

## 01.000.016 — 2026-09-25 · ⚠️ no ejecutable (crash diagnosticado)

- El runtime deja de matar la app en silencio: **stderr redirigido a
  `/download0/prospero-radio.log`** (descargable por FTP), rastro de las últimas 32
  asignaciones y `abort()` con el tamaño pedido en el log.
- **La prueba en consola dio el culpable**: `calloc(1, 1,22 MB)` del mapa de glifos de
  la fuente multilingüe devolvía NULL a los ~0,6 s. Demostró que el heap de libc no
  crece (ver `BUILD-FIX28.md`).

## 01.000.015 — 2026-09-25 · ⚠️ no ejecutable

- Controles funcionales: stick izquierdo = volumen, stick derecho = sintonizar,
  cruceta izquierda/derecha = recorrer listas, triángulo = ajustes. L1/R1 ya no cambian
  de lista.
- Limita a 16 MiB las lecturas completas de recursos y valida los recuentos del driver
  Vulkan. Sigue crasheando: el problema no estaba ahí.

## 01.000.010 → 01.000.014 — 2026-09-25 · ⚠️ no ejecutables

- Iteraciones de integración del driver Vulkan (enlace estático de Mesa/PSBC, bootstrap
  del PS5 OpenGL SDK 0.3.0, generados NIR/ACO, auditoría de símbolos). Build y
  empaquetado verificados, pero **cada lanzamiento moría en `SIGILL` silencioso**
  (`ud2` tras un malloc fallido) sin dejar ni una línea de diagnóstico: el `stderr` del
  título se descarta en consola.
- La 014 añadió las protecciones de lectura/recuento y descubrió (por telemetría) que
  la consola seguía ejecutando la 013 — de ahí la política de versionar y verificar
  cada build.

## Antes (base)

- `blackbearreloaded/ProsperoRadio` commit `33898dd` + paquete de interfaz 2.2.1.
  Funciona con su renderer SDL software original; este fork no altera su lógica de
  radio/catálogo/favoritos.

---

## Qué hay en el repo

| Ruta | Contenido |
| --- | --- |
| `overlay/` | Todo lo que este fork añade: renderer Vulkan, runtime de memoria, UI, scripts de parcheo |
| `BUILD-FIX1..28.md` | Historial técnico de cada corrección de build/crash |
| `CRASH-DIAGNOSIS-2026-09-25.md` | Forense completo de los 35+ crashes y cómo se simbolizaron |
| `out/` | Tooling de análisis (simbolizado de coredumps, test del pool, watchers de klog/FTP) |
| `src/`, `tooling/`, `vendor/`, `docs/` | El árbol base de upstream, conservado del fork original |

## Distribución

Los paquetes (`PPSA99001.ffpkg` / `PPSA99001.ffpfsc`) y sus SHA-256 se documentan en
`BUILD-FIX28.md`; no se versionan en git por tamaño — adjúntalos a un *Release* de
GitHub con la etiqueta de la versión.
