# Memoria del proyecto — ProsperoRadio Vulkan Edition

> Documento único que resume las etapas del proyecto, lo importante de cada una y las
> lecciones técnicas que costó aprender. El detalle fino de cada corrección quedó en
> ficheros de trabajo locales (no versionados); este documento es la referencia.

**Estado resumido (2026-09-26):** versión del título **01.000.017** compilada,
instalada y **ejecutable en consola PS5** (validada por el autor). Quedan pendientes
de pulido visual: descuadres/alineación de textos en la UI y validar los cambios de
modo de vídeo.

---

## 1. Por qué existe este fork

1. **Render por Vulkan.** El ProsperoRadio original dibuja la UI con SDL software;
   aquí se sustituye por PS5_Vulkan (Mesa → AGC), manteniendo SDL solo para mandos y
   temporización. Interfaz física de radio 3×2, fondo 4K en KTX2 leído por bloques,
   salida ajustada al modo de VideoOut activo.
2. **Memoria real de consola.** El heap de libc del título no crece más allá de unos
   pocos MB: toda asignación grande moría. Se añadió un **pool propio de Direct Memory
   de 512 MB** para las asignaciones grandes (≥ 256 KB) dejando el heap para las
   pequeñas.
3. **Diagnóstico en hardware real.** Log del runtime en `/download0/prospero-radio.log`
   (el stderr del título se descarta en la consola), rastro de las últimas 32
   asignaciones, backtraces simbolizados y tooling propio de análisis de coredumps.

La lógica del original (radio, catálogo, favoritos, streaming) no se altera: el fork
materializa el commit `33898dd` de upstream y aplica `overlay/` encima en cada build.

## 2. Cronología por etapas

### Etapa 1 — Integrar el driver Vulkan en el build (builds 01.000.010→013)

El trabajo duro fue el **enlace estático** del driver en `eboot.bin` (la consola no
permite cargar módulos gráficos con `dlopen`): compilar PSBC, el driver PS5 y enlazar
Mesa/ACO/NIR dentro del título.

- Bootstrap reproducible: clona el upstream fijado, aplica `overlay/`, compila en WSL2
  con fingerprint del overlay (cualquier cambio fuerza reconstrucción limpia).
- Bootstrap del **PS5 OpenGL SDK 0.3.0** (`ps5-opengl-sdk`, fetch de fuentes antes de
  compilar PSBC) y orden de `make deps` respetando el contrato de upstream.
- Correcciones de compilación Mesa (`u_format_gen.h`, archivos PSBC), resolución de
  símbolos duplicados (`__assert` entre `main.cpp` y `libpsbc_support`), auditoría
  post-link de símbolos indefinidos, robustez del parcheo de la política de warnings
  de Mesa (emparejamiento no literal + `git reset --hard` del checkout para evitar
  estado obsoleto entre ejecuciones) y anclas de enlace para las tablas generadas de
  NIR (`nir_intrinsic_infos`, `nir_op_infos`, …) que el auditor confundía con imports.
- Resultado: `eboot.bin` firmado íntegro, `ffpkg` UFS2 válido, `ffpfsc` sin warnings.

**Lo que fallaba en consola:** cada lanzamiento moría en `SIGILL` silencioso
(`ud2` tras un malloc fallido) sin dejar ni una línea — el `stderr` del título se
descarta en la consola.

### Etapa 2 — Forense de los crashes (35 volcados, builds 010→015)

- Captura sistemática: watcher FTP de coredumps (`/user/devlog/system/sce_coredumps.0/`)
  y monitor de klog. 35 crashes con firma idéntica: `rip=0x4001a9`, `rdi=rdx=0x8001d3cf0`.
- La clave fue **simbolizar**: el `eboot.bin` es SELF cifrado, pero el árbol de build
  en WSL conserva `build/llvm-pie.elf` (ELF pre-conversión con símbolos). Base del PIE
  0 con carga en runtime en 0x400000 → restar 0x400000 y `addr2line`/`nm` dan la cadena.
- Cadena real: `RunApp → Rml::Context::Render → carga de textura UI →
  ReadWholeFile → vector::resize → operator new → malloc → NULL → ud2`.
- Trampas del análisis descartadas: `rdi=0x8001d3cf0` **no** es un "malloc de 34 GB",
  es un puntero interno de libkernel (`SceKernelInternalMemory`); el límite de 16 MiB
  de `ReadWholeFile` sí estaba en el binario; el `stderr` de la app se perdía, así que
  ningún diagnóstico llegaba al klog.

### Etapa 3 — Diagnóstico cerrado (01.000.016)

- El runtime de asignación (`tooling/native/app_cpp_runtime.cpp`) ya no ejecuta `ud2`:
  redirige stderr a `/download0/prospero-radio.log`, guarda un rastro de 32 tamaños y
  aborta con reporte (`BUILD-FIX27`).
- La prueba en consola cambió el crash a `abort is called` y el backtrace nombró al
  culpable: **`calloc(1, 1,22 MB)` del crecimiento del mapa de glifos del .fnt
  multilingüe** (robin_hood llama `calloc` directo, sin pasar por `operator new`),
  a los ~0,6 s del lanzamiento. Conclusión: **el heap de libc no crece**; cualquier
  asignación grande muere, y cuál muere primero depende de milisegundos.

### Etapa 4 — La solución (01.000.017)

- **Pool de Direct Memory de 512 MB** (de los 2,25 GB concedidos al proceso): el
  enlace intercepta `malloc/calloc/realloc/free/posix_memalign` (`--wrap`); ≥ 256 KB →
  mapeos DMEM propios (tipo 12, protección 0x33, alineación 0x4000) con caché de
  reúso best-fit y spinlock; < 256 KB → heap de libc.
- **Registro de propiedad** (tabla hash) para clasificar punteros sin sondear memoria
  ajena — los chunks grandes de libc van por `mmap` y leer antes del puntero es
  SIGSEGV seguro (el test lo destapó).
- Semántica fijada: `calloc` siempre devuelve ceros, `realloc(p, 0)` libera y devuelve
  NULL, fallo → log con tamaño + rastro y `abort()`.
- Test unitario en host con mocks del kernel (estrés multihilo) — **PASSED** — y
  validación real: **la app arranca y funciona en consola**.

### Etapa 5 — Publicación y documentación

- Repo público `RastaFairy/Prospero_Radio_Vulkan`: fusión del árbol upstream con esta
  capa de modernización (`-X ours`, sin destruir historial), etiqueta `01.000.017`,
  `CHANGELOG.md` por versión y esta memoria.

## 3. Lecciones técnicas (lo que hay que saber para tocar esto)

- **Simbolizar coredumps PS5**: usar el `llvm-pie.elf` del worktree WSL (base PIE 0 →
  restar 0x400000 a las direcciones de runtime). Copias preservadas:
  `out/PPSA99001-vNNN-llvm-pie.elf`.
- **El stderr del título se descarta**: todo log va a `/download0/` (writable, y el
  FTP de etaHEN lo expone).
- **robin_hood llama `calloc` directamente**: un override de `operator new` no cubre
  sus tablas; de ahí el `--wrap` a nivel de enlace.
- **Nunca sondear memoria antes de un puntero ajeno** para clasificarlo: registrar la
  propiedad en una tabla propia.
- **El heap de libc no crece** en esta configuración de lanzamiento: cualquier feature
  nueva que pida bloques grandes debe ir al pool (bajar el umbral de 256 KB si hace
  falta).
- El `eboot.bin` de `recovery/` es SELF cifrado: los artefactos analizables son los
  `llvm-pie.elf` por build.

## 4. Estado actual y pendientes

| Tema | Estado |
| --- | --- |
| Instalación / arranque / ejecución en consola | ✅ funciona (01.000.017) |
| Streaming de radio, catálogo, favoritos | ✅ funciona (sin cambios de lógica) |
| Descuadres / alineación de textos en la UI | ⚠️ pendiente de pulido |
| Validación de modos de vídeo (4K / 1440p / 1080p) | ⚠️ por validar |
| Log de diagnóstico en `/download0/prospero-radio.log` | ✅ activo |

## 5. Dónde está cada cosa

**En el repo (público):**

- `README.md` — arranque rápido, versión y estado.
- `CHANGELOG.md` — historial por versión del título.
- `MEMORIA.md` — este documento.
- `overlay/` — todo lo que añade el fork (renderer, runtime, UI, scripts de parcheo).
- `out/` — solo el tooling conservado (ver abajo).
- `src/`, `tooling/`, `vendor/`, `docs/` — árbol base de upstream (heredado del fork).

**Solo local (no versionados, son ficheros de máquina):**

- `BUILD-FIX1..28.md`, `BUILD-FAILURE-FIX*.md`, `BUILD-FIXLOG.md`, `BUILD-STATUS.md`,
  `FIX4-REGRESSION.md` — el detalle paso a paso de cada corrección de build.
- `PROSPERORADIO_PROJECT_CONTEXT*.md` — contextos acumulativos por iteración.
- `CRASH-DIAGNOSIS-2026-09-25.md` — el forense completo (resumido en §2).
- `out/` resto: klogs, coredumps, logs de build, ELF simbolizables, scripts
  desechables de una sola pasada.
- `Administrador PowerShell.txt` — transcripción de sesión.

**Tooling conservado y versionado en `out/`:**

- `decompress_ps5_dump.py` — descompresor de `.prosperodmp` (LZ4).
- `disasm_ps5_core.py` — desensamblador de direcciones de runtime contra coredumps.
- `test_pool.cpp` — test unitario del pool (mocks del kernel, estrés multihilo).
- `monitor_ps5_klog.ps1` — captura de klog en vivo.
- `watch_ps5_coredumps.ps1` — watcher FTP de coredumps.
