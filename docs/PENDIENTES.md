# Pendientes de Prospero Radio

## Línea base y evidencia

- **02.000.054:** compilación local `PPSA99001` y gate del paquete aprobados
  (15 comprobaciones, 0 avisos, 0 fallos; MkPFS sin avisos ni errores). Al cambiar
  el foco CD a RADIO, se solicita parar cualquier reproducción iniciada desde el
  disco y se cancela el cambio de pista pendiente. Recuperar una emisora P1/P2/P3
  también duerme el panel CD y devuelve al LCD canónico el nombre de la emisora;
  el foco CD anterior bloqueaba ese refresco. La lista llama `archivos de audio`
  a los elementos extraídos de DVD y conserva `pistas` para CD-DA. Falta probar
  estos cambios en PS5.
  En el log de `.053` aparecen 256 archivos UDF escaneados y 34 reproducibles,
  todos MP3 en las solicitudes de reproducción registradas. 34 no es un límite
  del índice (máximo 128); el log no enumera los otros 222, así que no demuestra
  que sean canciones omitidas. WMA/M4A/MP4A aún no se decodifican.
- **02.000.053:** compilación local `PPSA99001` y gate del paquete aprobados
  (15 comprobaciones, 0 avisos, 0 fallos; MkPFS sin avisos ni errores). El
  payload configura una pila de 512 KiB para el trabajador óptico como
  mitigación del fault de pila observado al escanear UDF en la .052. Sigue
  pendiente repetir el DVD en PS5; el cambio de código y el gate no prueban que
  el crash esté corregido. WMA y M4A/MP4A continúan sin soporte de decodificación.
  Detalles y hashes en [`CHANGELOG.md`](../CHANGELOG.md).
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

### P1 — Corregir interfaz CD y navegación del mando en 02.000.050 y posteriores

**Abierto · fallo visible y funcional confirmado por el usuario en PS5.** La fotografía la captura aportada por el usuario muestra la interfaz con el sello 02.000.049 y estos problemas:

- La animación dibuja una fila de discos blancos muy brillantes que atraviesa la lista y el panel de estado, en vez de un único icono dentro de la pantalla CD. Debe quedar recortada al área del display y usar tonos ámbar/marrones de LCD antiguo. Comprobar también que solo se anime durante lectura/reproducción y que la pantalla CD se apague al devolver el foco a la radio; la causa técnica del desbordamiento todavía no está aislada.
- La botonera `SCAN / PREV / NEXT / STOP` aparece en la parte superior, aunque la propuesta validada deja esa zona sin botones. Retirar esos botones superiores y mantener la franja inferior de la radio como superficie canónica, sin cambiar su composición.
- El mando no sigue el contrato acordado: **arriba/abajo** cambia el foco entre la pantalla CD superior y la radio canónica inferior; **izquierda/derecha** recorre los botones de la franja inferior con la navegación existente. La lógica CD debe contextualizar los botones inferiores necesarios; no debe sustituirlos por una botonera superior ni interceptar la cruceta con un mapa inventado. Verificar las acciones CD de cada botón inferior y que, sin `cd1`, la radio conserve su funcionamiento actual.

**Actualización 02.000.050 — evidencia nueva de PS5.** El usuario facilitó cuatro fotos de esta build. Se ve la botonera inferior canónica (POWER/BANDA/MEMORIZAR/AUX/BARRIDO/EQ/PLAY-PAUSA); su comportamiento por botón aún requiere prueba. Las mismas fotos confirman desalineación de los paneles de madera y repetición del título de pista entre el módulo CD y el LCD de radio. En código, `generate-cd-ui-assets.py` reducía verticalmente una franja de madera de 504 px lógicos a 322 px y espejaba el lado derecho; `RefreshDiscScreen()` muestra el nombre de pista arriba mientras `RefreshHome()` lo repetía abajo. Se preparó una corrección de fuente para el siguiente candidato 02.000.051, pero no se compiló ni se validó en PS5. El paquete de referencia es `out/prospero-radio-02.000.050-ppsa-cd-controls/PPSA99001.ffpfsc`; su `param.json` y el banner visible en las fotos indican `02.000.050`.

**CDDA — lectura de sectores falla en parte de los medios.** La foto muestra un disco de 8 pistas CD-DA en estado `PLAYING` en v050. El `prospero-radio.log` local es concatenado: las sesiones de líneas 1 y 106 muestran runtime `.049`; una sesión nueva `.050` empieza en la línea 206. En esa sesión se indexan discos de 10, 19, 12, 2, 10, 8, 11 y 12 pistas, pero se registran fallos de reproducción. El log acompañante `radio/prospero-payload-probe.log` informa fallos SCSI `READ CD` durante lectura de sectores: sense `key=0x2 asc=0x3a ascq=0x2` en las líneas 17–50, y `key=0x5 asc=0x64 ascq=0` en 66–73. Una secuencia de 8 pistas (línea 90) precede un fallo al leer la pista 2 (líneas 95–105), conteo compatible con la foto, aunque sin ID del disco no se puede confirmar que sea el mismo. Esto sitúa al menos algunos fallos antes de recibir PCM en el decodificador; no demuestra incompatibilidad física permanente ni una causa de codec.

**Hallazgo confirmado en fuente para el siguiente candidato, validación de hardware pendiente.** El descriptor TOC de formato 0 devuelve `ADR` en los cuatro bits altos y `CONTROL` en los cuatro bajos (MMC-6, tabla 476). El puente y los tres sondeos locales extraían `entry[1] >> 4` como `CONTROL`. Se corrigió la extracción a `entry[1] & 0x0f` en `prospero_radio_data_bridge.c`, `prospero_usb_cdda_http_ro.c`, `prospero_usb_cdda_ro_probe.c` y `prospero_optical_ro_probe.c`. El error podía etiquetar pistas de datos como audio y dejar sin activar la ruta de montaje de discos mixtos. No explica por sí solo los errores `READ CD` ya registrados para discos de audio; hay que validar ambos tipos de medio en hardware.

**DVD de datos — formatos de audio abiertos.** La fuente del puente óptico solo indexa ficheros con extensión `.mp3` y solo después de montar la unidad como `cd9660` en `/mnt/disc`; por eso `.wma`, `.wav` y otros formatos no se presentan en la lista. En el log del puente también hay tres fallos `READ TOC` y un rechazo de montaje porque `/mnt` no pasó la verificación de ser el `tmpfs` esperado (líneas 106–116 de `radio/prospero-payload-probe.log`), seguidos por un índice vacío. Esto explica una ruta de fallo del escaneo de datos de esa sesión, pero el log no identifica el disco como DVD ni su sistema de archivos. El reproductor contiene decodificación para MP3, AAC, FLAC, OGG/Opus/Vorbis y WAV PCM de 16 bits con cabecera RIFF sencilla, pero no WMA ni contenedor M4A/MP4. No se debe confundir que exista un decodificador en la radio con que el puente lo indexe y transporte desde un disco. Falta definir formatos por extensión/MIME, compatibilidad de sistemas de archivos de DVD (incluido UDF) y una implementación de WMA o declararlo no soportado.

### Seguimiento — Microcortes de reproducción en vivo en 02.000.049

En la prueba de hardware el usuario indica que los microcortes **parecen eliminados**, pero quiere continuar probando. Tratarlo como mejora provisional, no como cierre: falta una escucha prolongada en varias emisoras y protocolos. La fuente de v049 incluye una línea acotada `[ProsperoRadio][audio-buffer]` con contadores de vaciado de cola y huecos entre envíos a AudioOut; todavía no hay un log de hardware que permita correlacionar esos valores con la escucha. Si reaparecen, conservar `/download0/prospero-radio.log` para exportarlo y compararlo con el klog de esa misma sesión.

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
