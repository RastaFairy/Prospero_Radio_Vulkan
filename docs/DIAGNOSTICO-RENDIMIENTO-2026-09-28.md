# Degradación tras reproducción prolongada — 2026-09-28

## Síntoma

El usuario informa que, después de varias horas de reproducción, Prospero Radio se vuelve extremadamente lenta. La observación corresponde a la revisión mostrada como `01.000.042`; no se dispone del hash del paquete instalado para vincularla con un artefacto binario concreto.

## Evidencia disponible

### klog de consola

Archivo local conservado en `out/prospero-radio-01.000.042/ps5_klog_2026-09-28_20-27-05.txt` (163.803 bytes). Contiene 1.006 mensajes `filesystem full` asociados a `pid 122 (eboot.bin)`, con marcas internas desde `20:25:31.621` hasta `20:25:52.456`.

El klog confirma fallos de escritura/espacio durante esa ejecución. No identifica la ruta, archivo ni montaje agotado, y por sí solo no demuestra que ese evento causara la lentitud observada durante horas.

En la misma captura, el bridge AUX registra que el servidor se detuvo y liberó el listener, que el cierre terminó con `exit result=0`, y que el proceso del bridge salió. Una muestra FMEM posterior ya no lista `ProsperoRadioDataBridge.elf`. El resultado `download0_rw=FAIL` también aparece en la línea final: se conserva como limitación del sondeo directo del payload; no equivale a que falle la comunicación de la app mediante el bridge.

### Log de runtime local

El archivo `out/prospero-radio-01.000.042/download0/prospero-radio.log` ocupa 11.598.333 bytes y contiene 29.019 avisos `No font face defined` para `Montserrat [bold]`, en elementos `chip-p0`, `chip-p1` y `chip-p2`.

Este archivo tiene metadatos de hora anteriores a los del klog y no hay evidencia que los identifique como la misma sesión. Es evidencia de una fuente de escritura repetitiva en una ejecución de v042, no prueba de que generara los 1.006 errores `filesystem full` del klog.

### Código fuente relacionado

- [`overlay/src/app_cpp_runtime.cpp`](../overlay/src/app_cpp_runtime.cpp): la fuente de v042 redirigía `stderr` a `/download0/prospero-radio.log` y lo dejaba sin búfer. En esa versión, la comprobación de 2 MiB solo se realizaba al inicio.
- [`src/main.cpp`](../src/main.cpp): carga las caras bitmap Montserrat en pesos normales.
- [`overlay/src/bitmap_font_engine.cpp`](../overlay/src/bitmap_font_engine.cpp): el cargador bitmap registra las caras encontradas; la ruta de `LoadFontFace` no remapea por sí misma la solicitud de peso.
- [`overlay/assets/ui/styles/app.rcss`](../overlay/assets/ui/styles/app.rcss): los estados de los chips de presets solicitan `font-weight: bold`.

## Diagnóstico

**Confirmado:** el usuario observa lentitud tras varias horas; el klog de la ejecución entregada contiene 1.006 errores `filesystem full`; hay un log local de v042 con 29.019 avisos de fuente y un tamaño de 11,6 MB; la fuente de v042 permitía que el archivo creciera durante la sesión porque solo aplicaba el límite al arrancar.

**Hipótesis principal:** la solicitud de `Montserrat [bold]`, no satisfecha por las caras cargadas, produce avisos repetidos. Al escribirse sin búfer en `/download0`, podrían contribuir al agotamiento del almacenamiento y a la degradación. Falta el log de runtime de la misma sesión del klog, y el klog no señala el montaje ni el archivo agotado; por eso no se debe presentar como causa raíz confirmada.

**No demostrado:** fuga de memoria, crecimiento de memoria por horas, agotamiento de un montaje concreto o correlación exacta entre esos avisos y el episodio de lentitud. La evidencia disponible es de filesystem; no muestra una tendencia de memoria de la app.

## Cambio de fuente incluido en 01.000.046

La fuente se compiló en v046 y el usuario confirmó que la interfaz mantiene una respuesta ágil durante su prueba. Esta observación no fue una sesión prolongada y no permite cerrar el diagnóstico de filesystem.

- La consulta de tipografía usa la cara Montserrat regular más cercana cuando RCSS solicita `bold` y no hay atlas bold cargado. Esto permite resolver la cara en vez de generar un aviso por frame; no se instaló un filtro para ocultar mensajes.
- El runtime vuelve a comprobar el tamaño de `stderr` desde el loop y reinicia el archivo al superar 2 MiB, conservando en él los mensajes recientes. La comprobación ocurre cada 30 frames; el tamaño puede superar el umbral entre comprobaciones.

El fallback está diseñado para evitar el aviso repetido de fuente identificado en la evidencia local y la rotación para acotar el crecimiento sostenido del archivo. No hay un log de runtime v046 emparejado con un klog prolongado que confirme que desaparecieron los avisos, que el log se mantuvo cerca del límite o que cesaron los `filesystem full`.

## Método de corrección y cierre

1. En una sesión prolongada de consola con v046, conservar juntos el log exportable `/download0/prospero-radio.log` y el klog, con hora de inicio/fin y versión/hash del paquete. Confirmar si desaparece el aviso de fuente, el log rota cerca del límite, aparecen errores `filesystem full` y la interfaz conserva su respuesta después de varias horas.

La v046 sí se compiló y se probó en PS5; esta revisión documental no compiló ni ejecutó pruebas.
