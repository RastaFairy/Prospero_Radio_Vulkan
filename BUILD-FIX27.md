# BUILD-FIX27 — Runtime de asignación con diagnóstico y salida controlada (01.000.016)

## Contexto del diagnóstico (2026-09-25, klog + coredumps)

Los 35 crashes registrados del 25-09 (v010→v015) comparten firma: `SIGILL` en
`rip=0x4001a9`. Con el `llvm-pie.elf` de la build de las 22:38 se simbolizó la pila:

```
RunApp → Rml::Context::Render → Rml::Element::Render → ElementText::OnRender
  → Geometry::Render → FileTextureDatabase::EnsureLoaded
  → Ps5VulkanRenderInterface::LoadTexture → ReadTextureFile
  → ReadWholeFile → out.resize(size) → vector<unsigned char>::__append
  → operator new (app_cpp_runtime.cpp) → malloc → NULL → __builtin_trap() (ud2)
```

Hallazgos clave:

1. El `ud2` es el `__builtin_trap()` de `allocation_failure()` en
   `tooling/native/app_cpp_runtime.cpp` (upstream): cualquier fallo de `malloc`
   mata el título sin dejar información.
2. El binario v015 **sí** contiene el límite de 16 MiB (`cmp $0x1000001` tras
   `ftell` en `ReadWholeFile`), así que el `resize` que estaba en la pila pedía
   ≤16 MiB: un malloc pequeño falló.
3. `rdi=rdx=0x08001d3cf0` en los 35 volcados **no es un tamaño**: es un puntero
   válido a las estructuras `SceKernelInternalMemory` de libkernel (la página
   `0x08001d3bf0` está volcada en los coredumps). El "malloc de 34 GB" de los
   informes anteriores era ese puntero dejado por el malloc interno que falló.
4. Los lanzamientos viven entre ~18 s y ~3 min antes del crash (huecos entre
   EXEC consecutivos: 18 s, 21 s, 2.9 min…), y la profundidad de pila varía
   entre volcados: el fallo no está en un único punto de llamada, apunta a
   agotamiento/rotura del heap del proceso durante el arranque de la UI.
5. El `stderr` del título se descarta en consola: todos los `fprintf(stderr,
   "[PS5-Vulkan] …")` del renderer (éxitos y fallos de carga de texturas)
   desaparecían sin rastro; por eso el klog no tiene ninguna línea de la app.

## Corrección (01.000.016)

`overlay/apply-vulkan.py` incorpora `patch_cpp_runtime()`, que reemplaza
`tooling/native/app_cpp_runtime.cpp` (idempotente; se re-aplica tras cada
`git reset` del worktree):

- **Canal de log real**: inicializador estático hace
  `freopen("/download0/prospero-radio.log", "a", stderr)` (rotura a 2 MiB).
  `/download0` es el montaje de datos que la app ya usa para favoritos y
  catálogo, y el FTP de etaHEN lo expone. Todos los diagnósticos existentes del
  renderer pasan a ser descargables.
- **Rastro de asignaciones**: anillo estático de 32 tamaños (sin asignar memoria
  en el camino del fallo).
- **`allocation_failure(size)`** en vez de trap silencioso: imprime el tamaño
  pedido + el rastro, `fflush` y `abort()` (SIGABRT en lugar de SIGILL; el
  crash reporter sigue actuando, pero el log queda escrito).
- **Guardia de pedidos absurdos**: `size > 1 GiB` devuelve `nullptr` sin tocar
  malloc (nothrow recibe `nullptr`; el camino con excepciones desactivadas pasa
  por `allocation_failure` con el tamaño en el log).
- `ps5ObserveOwnedAllocation` y la semántica de los `operator delete` se
  conservan intactos.

`VERSION` pasa a `01.000.016` (antes `01.000.015`) para que la consola distinga
la build en el klog (`contentVersion={01.000.016}`), evitando la ambigüedad que
hubo con la 01.000.014.

## Verificación

- `python -m py_compile overlay/apply-vulkan.py` — OK.
- AST extraído de `CPP_RUNTIME_SOURCE`: balance de llaves 0, sin comillas
  triples anidadas (lección de FIX25), `freopen`/`abort`/rastro presentes.
- `g++ -std=c++17 -fsyntax-only` sobre el C++ generado — OK.
- Build real `build.sh packages` en WSL2 Ubuntu 24.04 — **exit 0**.
- Cadenas del runtime parcheado confirmadas dentro del binario enlazado
  (`/download0/prospero-radio.log`, `[PS5-RT] runtime log ready…`,
  `[PS5-RT] FATAL: allocation of %llu bytes failed…`).
- `sce_sys/param.json` → `contentVersion: 01.000.016`.
- FFPFSC: PFS Check Report — **Warnings: 0, Errors: 0**, 1 fichero
  hash-checked, imagen final 53.56 MB verificada al 100% (CRC/manifest OK).
- Manifest SHA256 del PFS: `df7bab73078ff2eb9d7a2d432f2b81e7ad4680160c177c19708067b16d462faf`.

## SHA-256 del resultado (out/, 2026-09-25 23:58)

- `eboot.bin`: `DD4ECF6303697028403FE2DDDAFCAC1BE361E5DA4EB7059817A76008746F1316`
- `PPSA99001.ffpkg`: `0473F985FCC17732A4CAED9BABE25DA7876C3A4FD3F6E538A9CE8199C8525BD9`
- `PPSA99001.ffpfsc`: `8834B8D3B50920F0D10E4FD41626DB4D3E94C3354569C0FD04BC6861143BFE2F`

## Qué esperar en la siguiente ejecución en PS5

1. Instalar el FFPKG de 01.000.016 y lanzar.
2. Descargar `/download0/prospero-radio.log` por FTP:
   - si el arranque es limpio, contendrá `[PS5-RT] runtime log ready…` y las
     líneas `[PS5-Vulkan] loaded KTX2 …` de cada textura;
   - si vuelve a fallar un malloc, la app aborta después de escribir
     `[PS5-RT] FATAL: allocation of N bytes failed … Recent allocations: …`,
     que identifica el subsistema y el tamaño exacto.
3. Si el crash desaparece, el ciclo instalar→crash→desinstalar queda roto; si
   persiste, el log indica la corrección puntual que sigue.
