# Pendientes de Prospero Radio

## Línea base y evidencia

- **01.000.046:** build probada por el usuario en PS5 y publicada manualmente.
  La interfaz se mantiene ágil, se reconoce la configuración previa en
  `/data/radio` y la cruceta arriba/abajo recorre la lista al mantenerla
  pulsada. DASH test 06 produce ruido blanco sin crash reportado en esta
  ejecución; algunos streams HLS siguen fallando. El paquete local coincide
  con el asset publicado por SHA-256 y pasó el gate de recursos (14 aprobados,
  0 avisos, 0 fallos). Esto no valida la reproducción DASH/HLS ni cada caso de
  AUX. Véase [`CHANGELOG.md`](../CHANGELOG.md).
- Las capturas compartidas por el usuario muestran `01.000.039` en pantalla. Esa es la build de los síntomas: lista M3U con textos superpuestos, ajustes sobre fondo claro, pestaña AUX poco visible y estado de preset memorizado que no se distingue bien. No hay en el `out/` actual un paquete v039 con hash que vincule esas capturas a un artefacto local.
- `01.000.041` se trató como candidata, pero no hay paquete v041 disponible en el `out/` actual ni una prueba atribuible a ella.
- `01.000.042` se compiló y el gate del paquete recién ensamblado pasó: 14 aprobados, 0 warnings y 0 fallos. Las fotos del usuario del 28-09-2026 muestran el sello v042 en PS5; no se cotejó el hash del paquete instalado, así que es evidencia parcial, no una certificación completa.
- El usuario confirmó previamente que EQ, reproducción, lista de estaciones más allá de la primera página, favoritos del catálogo, lógica de mando y color RGB del DualSense funcionan en las revisiones que probó. No inferir que toda esa evidencia corresponde a v042.
- El usuario informa una degradación grave de rendimiento tras varias horas de reproducción. El klog de v042 registra 1.006 errores `filesystem full` del proceso de la app entre 20:25:31.621 y 20:25:52.456. El klog no identifica el archivo o montaje afectado; la posible relación con el log de runtime sigue siendo una hipótesis. Ver [`DIAGNOSTICO-RENDIMIENTO-2026-09-28.md`](DIAGNOSTICO-RENDIMIENTO-2026-09-28.md).
- No reintroducir ajustes ni selector de temas: el usuario los descartó. La detección y representación del jack también quedan fuera del alcance; no abrir tareas para perseguirlas ni presentar `JACK N/A` como detección física.

## Pendientes por prioridad

### P1 — Evitar agotamiento del sistema de archivos y lentitud tras varias horas

**Complejidad media · impacto alto.** El usuario observó que, tras varias horas de reproducción, la aplicación se vuelve extremadamente lenta. El klog de consola asociado a la revisión v042 contiene 1.006 mensajes `filesystem full` para `pid 122 (eboot.bin)`, desde 20:25:31.621 hasta 20:25:52.456. Como evidencia separada, el `prospero-radio.log` local de v042 ocupa 11.598.333 bytes y contiene 29.019 avisos repetidos de fuente `Montserrat [bold]` no definida; su hora/ejecución no coincide con el klog, por lo que no demuestra que ese log causara aquellos errores. La fuente de v046 incluye fallback a la cara Montserrat regular y rotación del log al superar 2 MiB durante el loop (comprobación cada 30 frames), además del límite al arranque. El usuario informa que v046 mantiene la agilidad en su prueba, pero no consta una sesión prolongada con cotejo de tamaño, klog y ausencia de `filesystem full`; no se declara cerrado. Diagnóstico y evidencia en [`DIAGNOSTICO-RENDIMIENTO-2026-09-28.md`](DIAGNOSTICO-RENDIMIENTO-2026-09-28.md).

### P1 — Validar la importación AUX y sus formatos en hardware

La fuente incluida en v046 admite M3U/M3U8, PLS, XSPF y ASX, conserva el tipo junto al contenido y filtra URLs repetidas dentro de un fichero. El cuerpo se limita a 4 MiB tanto en el servidor como en el puente/parser. La lista AUX se transfiere en memoria entre app y payload y se persiste en `/data/radio/radio-aux.m3u`; la app ya no usa una copia `/download0/radio-aux.m3u` para leer o editar esa lista. Falta probar cada formato en PS5, incluidas codificaciones, metadatos y URLs relativas, y confirmar reimportación, contador y persistencia tras cerrar/reabrir. La copia de trabajo `/download0/radio-aux-favorites.bin` es independiente de la lista AUX y continúa en el flujo de favoritos. Véase [`PUENTE-PAYLOAD.md`](PUENTE-PAYLOAD.md).

### P1 — Borrado deliberado de una emisora AUX

La fuente y la build v046 implementan mantener **Cuadrado** durante 3 segundos con cuenta atrás visible; al completar, guardan la lista editada por el puente en memoria. Falta validar en consola la cuenta atrás, cancelar al soltar antes, borrar solo la emisora seleccionada y comprobar el resultado después de volver a leer la lista persistente.

### P1 — Corregir la reproducción DASH y los fallos HLS

**Abierto · causa no determinada.** El test DASH 06 (`https://livesim.dashif.org/vod/WAVE/av/combined.mpd`) se reportó como crash con v045; en la v046 el usuario oye ruido blanco y no reporta crash en esta ejecución. El cambio de síntoma no confirma reproducción audible correcta ni permite atribuir una causa. Algunos streams HLS continúan fallando según la prueba del usuario. Revisar el log exportable `/download0/prospero-radio.log` y el klog correspondientes a la misma ejecución, acotar fase y codec y conservar evidencia; el muestreo parcial de logs por sí solo no es un arreglo.

### Observación visual — Etiquetas de las fuentes en el LCD

Las capturas v042 muestran el rótulo central vacío en una vista y `MEM / FAVORITOS` solapado con `AUX M3U`. La fuente posterior ajustó el espacio de las pestañas. El usuario informa que la v046 se ve decentemente y no observa otros errores aparentes fuera de protocolos; no hay una revisión visual documentada de cada vista, así que reabrir solo ante una reproducción concreta.

### Observación visual — Contraste del encabezado web AUX

La captura histórica mostraba bajo contraste del texto de modelo sobre el metal. La fuente posterior lo cambió a texto claro con sombra oscura. No hay comprobación actual separada de móvil/PC; el usuario no ha comunicado una regresión.

### Decisión — Detección del jack fuera del alcance

El usuario descarta continuar la investigación o implementación de la detección visual de auriculares del DualSense: el coste no compensa. El registro de v042 (`scePadGetJackState not exported`) y la etiqueta observada `JACK N/A` quedan como evidencia histórica, no como fallo pendiente. No añadir exploración de APIs, cambios de UI ni validaciones de conexión para este tema salvo que el usuario reabra expresamente el alcance.

## Validaciones recientes

- **Favoritos y presets AUX:** el usuario indica que el problema parece corregido y que ahora funciona. Retirado de pendientes a la espera de una regresión concreta.
- **Cierre del payload y servidor AUX:** el klog de v042 registra `AUX server stopped`, `bridge shutdown complete`, `process exit result=0` y no muestra el módulo del bridge en la muestra FMEM posterior a la salida. Esto valida el cierre en esa ejecución; `download0_rw=FAIL` sigue siendo un resultado explícito del sondeo directo del payload y no debe confundirse con el acceso mediante la API puente. La persistencia de los datos después de reiniciar no queda demostrada por este klog.
- **01.000.046 en PS5:** el usuario confirma configuración previa visible desde `/data/radio`, navegación sostenida arriba/abajo y respuesta ágil. Informa ruido blanco en DASH 06 y fallos de algunos HLS; no comunica crash en esa ejecución DASH. No se aportó en esta observación un nuevo klog/runtime que identifique fase o codec.

## Diferencias registradas

- **01.000.039:** build mostrada en las capturas del usuario; lista/textos, ajustes y señalización de presets presentaron los síntomas anteriores.
- **01.000.041:** candidata documentada para selector de fichero M3U y cambio de fuente en RADIO; no hay paquete v041 ni resultado de consola en el `out/` actual.
- **01.000.042:** capturas en consola confirman el sello de versión, emisora y listas visibles. En esas capturas se observan las pestañas defectuosas y `JACK N/A` (la detección del jack quedó fuera del alcance posteriormente); el usuario reporta duplicados tras reimportar una M3U y lentitud tras horas de reproducción. Un klog informa errores `filesystem full`; un log de runtime de otra hora contiene avisos de fuente repetidos. No hay correlación temporal que confirme la causa. El gate local pasó, pero la validación de hardware es parcial.
- **01.000.045:** test DASH 06 reportado por el usuario como crash de app/sistema. Evidencia local histórica descrita en el documento de continuidad; no usar como resultado de v046.
- **01.000.046:** gate local aprobado, hash del paquete igual al asset publicado y prueba del usuario en PS5. UI ágil, configuración previa accesible en `/data/radio` y navegación sostenida confirmadas por el usuario; DASH 06 da ruido blanco sin crash reportado y algunos HLS fallan. AUX en memoria, formatos, gesto de borrado, audio DASH/HLS y sesión prolongada siguen sin cierre específico de hardware.

Registrar por separado código, paquete, hash, gate, klog y observación en PS5. La etiqueta en pantalla identifica la versión mostrada, no el hash instalado; el gate no demuestra audio, acceso LAN ni persistencia en `/data`.
