# Diagnóstico de crashes — ProsperoRadio (PPSA99001) — 2026-09-25

## Resumen ejecutivo

La app **arranca y se instala bien, pero todo lanzamiento de ProsperoRadio termina en
`SIGILL` (señal 4, "privileged instruction fault")**, en las versiones 01.000.010 a
01.000.015 por igual. Las protecciones añadidas en 01.000.014/015 (límite de 16 MiB en
lecturas y validación de recuentos Vulkan) **no tocaron la ruta que crashea**: la firma
del crash es idéntica en 013 y en 015. Desinstalar/reinstalar no arregla nada — el klog
muestra 20 desinstalaciones de PPSA99001 y el mismo crash tras cada reinstalación.

**ProsperoTV (PPSA99003), que comparte entorno de despliegue, NO crashea** (termina con
`ProcessTerm` normal). El fallo es específico del camino de datos de la radio, no del
motor Vulkan compartido.

## Evidencia en logs (`out/`)

| Archivo | Qué demuestra |
| --- | --- |
| `out/klog-20260925-151657.log` (4 MB) | 35× `# signal: 4 (SIGILL)`, 35 coredumps, 20 desinstalaciones `UP9000-PPSA99001_00-PS5RADIOAPP00001`. Ciclo completo instalar→lanzar→crash→desinstalar repetido en v010–v015. |
| `out/ps5_klog_live_2026-09-25_19-46-37.txt` | Captura en vivo 19:46→22:57 del mismo comportamiento. |
| `out/captured_coredumps/*.prosperodmp(.elf)` | 13 volcados capturados por FTP watcher (19:48→22:53). En `ftp-watch.status.txt` consta un download fallido del coredump 0x108 con reintento. |
| klog líneas 123–175, 42031, 45774, 46544, 47438, 49335… | Volcado de registros de cada crash: `rip=0x4001a9`, `rdi=rdx=0x08001d3cf0`, `App Crash : PID=…, reason=0x4`, `SCE_SHELL_UTIL_ERROR_APPLICATION_CRASH`. |

Firma constante en todos los crashes (v010→v015):

```
# signal: 4 (SIGILL)
# reason: privileged instruction fault
# rip: 00000000004001a9
# rdi: 00000008001d3cf0   ← mismo "tamaño" absurdo en todos
# rdx: 00000008001d3cf0
```

## Funciones erróneas identificadas

### 1. Wrapper de malloc chequeado @ `eboot 0x400190` — el mecanismo del crash

Desensamblado desde la memoria del coredump `prosperocore-1790369650-0x0000010e…​.elf`
(el texto del eboot cifrado no es legible, pero el volcado de memoria sí):

```asm
0x400190: push rbp; mov rbp, rsp
0x400194: cmp  rdi, 1
0x400198: adc  rdi, 0            ; rdi = max(rdi, 1)
0x40019c: call [malloc]          ; rax = malloc(rdi)
0x4001a2: test rax, rax
0x4001a5: je   0x4001a9          ; si NULL →
0x4001a7: pop rbp; ret
0x4001a9: ud2                    ; ← SIGILL. Sin mensaje, sin cleanup.
```

Cualquier fallo de `malloc` se convierte en crash instantáneo. Es el "fusible" que
mata la app, pero no la causa raíz.

### 2. Ruta real del fallo (simbolizada con `build/llvm-pie.elf` de la build 22:38)

El backtrace completo, simbolizado con el ELF pre-conversión (con símbolos) del
árbol WSL:

```
RunApp (main.cpp)
  → Rml::Context::Render → Rml::Element::Render (recursivo)
    → ElementText::OnRender → Geometry::Render → RenderManager::Render
      → FileTextureDatabase::GetHandle/EnsureLoaded
        → Ps5VulkanRenderInterface::LoadTexture → ReadTextureFile
          → ReadWholeFile → out.resize(size) → vector<unsigned char>::__append
            → operator new (app_cpp_runtime.cpp:0x400190) → malloc → NULL → ud2
```

El binario v015 **sí** contiene el límite de 16 MiB (`cmp $0x1000001` tras
`ftell`), así que el `resize` en la pila pedía ≤16 MiB y falló igualmente.
Además `rdi=rdx=0x08001d3cf0` en los 35 volcados **no es un tamaño**: es un
puntero válido a las estructuras `SceKernelInternalMemory` de libkernel (la
página `0x08001d3bf0` está volcada en los coredumps). El "malloc de 34 GB" de la
primera hipótesis era ese puntero interno. Los lanzamientos viven entre ~18 s y
~3 min antes de crashear y la profundidad de pila varía entre volcados: el
fallo apunta a agotamiento/rotura del heap del proceso durante el arranque de la
UI, no a un único punto de llamada.

Descubrimiento añadido: el `stderr` del título se descarta en consola — todos
los diagnósticos `fprintf(stderr, "[PS5-Vulkan] …")` del renderer se perdían.
No había ninguna línea de la app en el klog.

### 4. Funciones del overlay revisadas y descartadas como punto de crash directo

- `Ps5VulkanRenderInterface::RenderGeometry` — defensivo, escribe en buffers
  pre-asignados con límites `max_vertex_bytes_`/`max_index_bytes_`; no hace malloc.
- `ReadWholeFile`, enumeraciones Vulkan (`IsReasonableVulkanCount`) — son las
  protecciones de 014/015; correctas pero irrelevantes: la firma del crash no cambió.

## Síntoma "no vuelve salvo desinstalando"

El klog muestra que relanzar tras un crash **sí ejecuta de nuevo el binario** (hay
EXECs consecutivos sin desinstalación entre medias) y vuelve a crashear con la misma
firma. La percepción "está muerto" viene de que el crash es inmediato tras mostrar la
UI; reinstalar solo devuelve el splash/arranco limpio y el ciclo se repite. No hay
evidencia de bloqueo del AppDb ni del montaje etaHEN como causa: el ciclo
desinstalar/reinstalar queda registrado 20 veces sin cambiar el resultado.

## Entorno (contexto de los logs)

- Consola PS5 con etaHEN + payload sonic-loader (trophy-unlock daemon) y
  `apr-emu-updater`; el FFPKG se despliega como `/mnt/ext1/etaHEN/games/PPSA99001.ffpfsc`
  montado por bfs (`/mnt/shadowmnt/PPSA99001_…`).
- Diálogo de error del sistema tras cada crash: `launchApp(NPXS40093)` + proceso
  `common_dialog` (por eso aparecen EXEC de eboot.bin `[system]` junto a cada crash).

## Metas (orden de prioridad)

1. ~~Simbolizar el backtrace~~ **HECHO (2026-09-25)**: `build/llvm-pie.elf` del
   árbol WSL conserva símbolos y DWARF; la cadena completa está resuelta arriba.
2. **Corrección aplicada en 01.000.016** (`BUILD-FIX27.md`): el runtime de
   asignación ya no ejecuta `ud2`; redirige stderr a
   `/download0/prospero-radio.log`, guarda un rastro de 32 tamaños y reporta el
   tamaño pedido antes de `abort()`. La siguiente ejecución en PS5 deja el
   diagnóstico exacto en un fichero descargable por FTP.
3. Sospechosos a auditar con el log nuevo: presión/agotamiento del heap durante
   el arranque de la UI (catálogo, audio, RmlUi) — el rastro de tamaños
   identificará al responsable.
4. Mantener el versionado estricto (01.000.016) para confirmar en el klog qué
   binario ejecuta la consola en cada prueba.
