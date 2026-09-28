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

- [`overlay/src/app_cpp_runtime.cpp`](../overlay/src/app_cpp_runtime.cpp): redirige `stderr` a `/download0/prospero-radio.log` y lo deja sin búfer. La comprobación/rotación de 2 MiB se realiza al inicio del proceso; no vuelve a ejecutarse mientras la app permanece abierta.
- [`src/main.cpp`](../src/main.cpp): carga las caras bitmap Montserrat en pesos normales.
- [`overlay/src/bitmap_font_engine.cpp`](../overlay/src/bitmap_font_engine.cpp): el cargador bitmap registra las caras encontradas; la ruta de `LoadFontFace` no remapea por sí misma la solicitud de peso.
- [`overlay/assets/ui/styles/app.rcss`](../overlay/assets/ui/styles/app.rcss): los estados de los chips de presets solicitan `font-weight: bold`.

## Diagnóstico

**Confirmado:** el usuario observa lentitud tras varias horas; el klog de la ejecución entregada contiene 1.006 errores `filesystem full`; hay un log local de v042 con 29.019 avisos de fuente y un tamaño de 11,6 MB; el logger permite que el archivo crezca durante la sesión porque solo aplica el límite al arrancar.

**Hipótesis principal:** la solicitud de `Montserrat [bold]`, no satisfecha por las caras cargadas, produce avisos repetidos. Al escribirse sin búfer en `/download0`, podrían contribuir al agotamiento del almacenamiento y a la degradación. Falta el log de runtime de la misma sesión del klog, y el klog no señala el montaje ni el archivo agotado; por eso no se debe presentar como causa raíz confirmada.

**No demostrado:** fuga de memoria, crecimiento de memoria por horas, agotamiento de un montaje concreto o correlación exacta entre esos avisos y el episodio de lentitud. La evidencia disponible es de filesystem; no muestra una tendencia de memoria de la app.

## Método de corrección y cierre

1. Corregir la correspondencia entre los pesos solicitados por RCSS y las caras Montserrat realmente registradas. Mantener el estilo visual deseado cargando la cara adecuada o ajustando la solicitud de peso al recurso disponible; no suprimir los avisos.
2. Hacer que la rotación/límite del log se aplique durante toda la sesión y que conserve el tramo reciente útil. El logger no debe poder generar escrituras ilimitadas si un aviso se repite.
3. En una sesión prolongada de consola, conservar juntos el log de runtime y el klog, con hora de inicio/fin y versión/hash del paquete. Confirmar que desaparece el aviso de fuente, el tamaño del log permanece acotado, no aparecen errores `filesystem full` y la interfaz conserva su respuesta después de varias horas.

No se modificó el runtime ni se compiló una build durante la documentación de este diagnóstico.
