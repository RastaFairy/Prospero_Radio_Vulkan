# BUILD-FIX28 — Pool de memoria dinámica Direct-Memory (01.000.017)

## Qué mostró la primera ejecución de la 01.000.016 (klog 23:59)

Dos lanzamientos, dos crashes **idénticos y deterministas a los ~0,6 s**, ya no
con `ud2` sino con `abort is called(system)` — la instrumentación de FIX27
funcionó y el backtrace quedó con nombre al simbolizarlo contra
`out/PPSA99001-v016-llvm-pie.elf`:

```
main → RunApp() → BitmapFontEngine::LoadFontFace (+0xf98)
  → robin_hood::Table<Rml::Character, BitmapGlyph>::increase_size()
  → doThrow<std::bad_alloc>() → abort()
```

Decodificando `increase_size` y los registros del crash:

- `r15 = 0x4000` = capacidad actual del mapa de glifos (16.384).
- `r14 = 0x80FF = 33.023` y `r12 = 9×33.023 = 0x488F7` = parámetros del nuevo
  bloque (`lea (%r14,%r14,8),%r12` en el desensamblado) — coincidencia exacta.
- La llamada que falla es **`calloc(1, 37×33.023+8 = 1.221.859)`**: robin_hood
  usa `calloc` directo (por eso el `operator new` propio no aparecía en la
  pila), y con `calloc == NULL` lanza `doThrow<bad_alloc>` → abort.

El mapa de glifos del `.fnt` multilingüe (17.854 caracteres, familia
"Montserrat" compartida) se queda sin sitio a mitad del parse y pide 1,2 MB.
Con la 01.000.015 el mismo arranque falló un paso después (la lectura del atlas
RTA de 1,6 MB): **el heap de libc del proceso no crece más allá de unos pocos
MB**, así que cualquier asignación grande muere, y cuál muere primero depende
de milisegundos. Las 35 muertes de las builds 010→015 y las 2 de la 016 son el
mismo problema.

## Corrección: pool propio de Direct Memory (petición de diseño del usuario)

`app_cpp_runtime.cpp` (ahora fichero real en `overlay/src/`, copiado por
`patch_cpp_runtime()`) añade:

- **Pool respaldado por Direct Memory** (`sceKernelAllocateDirectMemory` +
  `sceKernelMapDirectMemory`, tipo 12 / protección 0x33 / alineación 0x4000,
  las mismas constantes del driver): cada asignación grande recibe su propio
  mapeo; los bloques liberados se guardan en una caché de reutilización
  (best-fit) y el techo total es **512 MB** de los 2,25 GB concedidos.
- **Intercepción transparente** con `--wrap` del enlazador sobre
  `malloc/calloc/realloc/free/posix_memalign`: las asignaciones ≥ 256 KB van al
  pool; las pequeñas siguen en el heap de libc, que para eso sí funciona. Los
  punteros ajenos (asignados internamente por libc) se detectan por un
  **registro de propiedad** (tabla hash de bloques vivos) y se reenvían al
  allocator real — nunca se sondea memoria ajena.
- `calloc` garantiza ceros (incluido el reúso de bloques), `realloc` preserva
  contenido y trata `size==0` como free+NULL de forma consistente, y todo el
  pool está protegido por un spinlock (los hilos de audio/UI allocan en
  paralelo).
- Se mantiene todo lo de FIX27: log en `/download0/prospero-radio.log`, rastro
  de 32 tamaños, guardia de 1 GiB y abort con diagnóstico.

## Validación

- Test unitario en host (`out/test_pool.cpp`) con mocks del kernel:
  calloc grande con ceros, no-solapamiento, realloc con preservación, reúso de
  caché sin mapear de nuevo, small→libc, y estrés multihilo (4 hilos × 20.000
  operaciones con tamaños mezclados) — **ALL POOL TESTS PASSED** (arena
  ejercitada: 81 MB). El test detectó dos bugs reales antes del build (sondeo de
  memoria ajena con chunks mmap de glibc → registro de propiedad; semántica de
  `realloc(p,0)`).
- `python -m py_compile overlay/apply-vulkan.py` — OK.
- El binario enlazado contiene los 5 símbolos `__wrap_*` y las cadenas del pool.
- Build real, empaquetado y PFS check: **Warnings: 0, Errors: 0**, exit 0.

## SHA-256 del resultado (out/, 2026-09-26 01:00)

- `eboot.bin`: `0207902D1FA590EF279A9F3C67E0A7152950FEB5988ACF4A503D74A1E8AD7F18`
- `PPSA99001.ffpkg`: `B74B9F20082DE148297AC4DCC5F37382D86050C1A0CFE15D0B71FC95BB589009`
- `PPSA99001.ffpfsc`: `9DDCF315E34B800443BD1CCCBD2A697EC3F11AF670B9194CA7C900870C539295`

## Qué esperar en la siguiente ejecución en PS5

1. Instalar la 01.000.017 y lanzar.
2. `/download0/prospero-radio.log` debe mostrar el banner del runtime y las
   cargas de texturas. Si una asignación fallara aún, el log nombra el tamaño y
   el rastro; con el pool el heap de libc queda reservado para asignaciones
   pequeñas, que son las que la consola sí sirve.
