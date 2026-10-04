# Registro de cambios — Prospero Radio Vulkan

> **Fuente 02.000.055** · referencia publicada; paquete y validación en PS5 no confirmados en esta actualización.
>
> No confundir: *2.2.1* es la versión del paquete de interfaz original sobre el que se
> construye este fork; *02.000.0XX* es la `contentVersion` que ve la consola en
> `sce_sys/param.json` (la fija `overlay/apply-vulkan.py` en cada build).
>
> El flujo de compilación y sus límites se describen en
> [`VULKAN-INTEGRATION.md`](VULKAN-INTEGRATION.md). Los fallos pendientes actuales
> están en [`docs/PENDIENTES.md`](docs/PENDIENTES.md).

---

## 02.000.055 — 2026-10-04 · robustez bridge, detección de medios y cola AUX

- **CD/USB:** conserva el último índice válido ante respuestas incompletas o errores;
  valida estados, recuentos e IDs duplicados; detiene la reproducción al confirmar
  que el medio ya no está disponible.
- **Bridge:** aplica espera creciente al reconectar; el ping tiene un límite de dos
  segundos y el sondeo periódico es más frecuente. La sincronización inicial reintenta
  una vez y trata los archivos inexistentes en una instalación nueva como estado
  inicial válido.
- **AUX HTTP:** importaciones en cola acotada, ejecutadas fuera del bucle RPC, con
  escritura en un temporal distinto al usado por RPC. Se rechazan longitudes HTTP
  duplicadas y Transfer-Encoding.
- **Bridge (montajes/listener):** conserva constancia de montajes propios cuya
  liberación falla; reintenta abrir el listener USB si no pudo iniciarlo. Los
  tamaños de registro CD/USB están declarados en sus cabeceras de protocolo.
- El overlay ahora copia las cabeceras de protocolo CD/USB al árbol de compilación;
  el intento de empaquetado de esta actualización no se completó y no hubo prueba en PS5.
- **Limitaciones conocidas:** si la sincronización inicial falla tras los dos intentos,
  la sesión continúa con caché local o valores predeterminados (sin recarga SQLite en
  caliente). WMA/M4A no incluidos en estos cambios.
## 02.000.054 — 2026-10-03 · parada al volver a radio y etiqueta de archivos

- Al bajar desde el foco CD, la app solicita detener una reproducción iniciada
  desde el disco y cancela una transición de pista pendiente. La pantalla CD
  queda en reposo; falta confirmar en PS5 que el audio se detenga en esa transición.
- Al recuperar una emisora guardada en P1/P2/P3, la app abandona también el
  foco CD, duerme su pantalla y deja que el LCD canónico muestre la emisora.
  `RefreshHome()` podía devolver antes de pintar la emisora porque el foco CD
  seguía activo aunque `RecallPreset()` ya hubiera seleccionado otra fuente.
- Los elementos de discos de datos se etiquetan como `FILE`/`AUDIO FILES`; los
  CD-DA se mantienen como pistas, y discos mixtos se muestran como elementos
  de audio. El registro `.053` halló 256 archivos en el DVD y catalogó 34 como
  reproducibles (tipo MP3); no alcanzó el límite de 128 entradas. El log no
  clasifica los otros 222 por extensión, y WMA/M4A siguen sin decodificador.
- Build FFPFSC `PPSA99001` en
  `out/prospero-radio-02.000.054-ppsa-cd-usb-final/`: 50.135.040 bytes,
  SHA-256 `3066C1D729B6DF3C0306AF3561EE2DB89D026887B5CE0D036E01322BE6AF5875`.
  `eboot.bin`: 27.316.348 bytes, SHA-256
  `4B4285E5DCB492A913E59F07D56DD63F2D5190FC91FA1838F085358D9E8B2B0E`.
  `ProsperoRadioDataBridge.elf`: 194.120 bytes, SHA-256
  `5396BCF468B140C09CF4E79893DF9731A792D9A7755C5A4353EABC168AA8F0F6`.
- `param.json`, RML y banner runtime indican `02.000.054`; Title ID `PPSA99001`.
  El gate pasó 15 comprobaciones, 0 avisos y 0 fallos. MkPFS verificó la imagen
  con 0 avisos y 0 errores. No se ejecutaron pruebas independientes ni se ha
  validado esta build en PS5.

## 02.000.053 — 2026-10-03 · límites de pila para lectura óptica

- El hilo óptico del payload configura una pila de 512 KiB mediante
  `pthread_attr_setstacksize`; el ELF compilado contiene la llamada y el mensaje
  de inicio que informa del tamaño. La medida aborda el crash de UDF observado
  en la .052, cuyo fault de escritura cayó en la pila durante el escaneo. Es una
  mitigación basada en la ruta observada; falta repetir el DVD en PS5 para
  confirmar que evita el crash.
- La lista UDF conserva los tipos que el lector ya reconoce: MP3, AAC/ADTS,
  FLAC y WAV. WMA y M4A/MP4A siguen sin decodificador integrado; no se anuncian
  como reproducibles.
- Build FFPFSC `PPSA99001` en
  `out/prospero-radio-02.000.053-ppsa-cd-usb-final/`: 50.135.040 bytes,
  SHA-256 `B41AC022F72D646611968AEDDC98C87BF3C07245CC32809FF84862E8F327DDF9`.
  `eboot.bin`: 27.315.180 bytes, SHA-256
  `01F0200DF5659230C1D75A55B5CDDA8467FB58443D34F8AB9541BB0070D45A7E`.
  `ProsperoRadioDataBridge.elf`: 194.120 bytes, SHA-256
  `5396BCF468B140C09CF4E79893DF9731A792D9A7755C5A4353EABC168AA8F0F6`.
- `param.json`, versión visible en RML y banner runtime indican `02.000.053`.
  El gate del paquete pasó 15 comprobaciones, 0 avisos y 0 fallos; MkPFS
  verificó la imagen con 0 avisos y 0 errores. No se ejecutaron pruebas
  independientes ni se ha validado esta build en PS5.

## 02.000.052 — 2026-10-03 · soporte de DVD/UDF y marca del proyecto

- La app integra la lista de archivos de audio que expone el payload para la
  unidad óptica externa `cd1`, con reproducción de archivos indexados desde
  UDF mediante el flujo HTTP de solo lectura. La compilación confirma que el
  payload y la app se enlazan; el funcionamiento en PS5 sigue pendiente.
- El paquete toma `icon0.png`, `pic0.dds` y `pic1.dds` de `nuevas_imagenes/`;
  sus hashes coinciden con los tres recursos incluidos. El `titleName` y el
  título RML pasan a **Prospero Radio Vulkan**. Se conserva `PPSA99001`.
- Build FFPFSC en
  `out/prospero-radio-02.000.052-ppsa-cd-usb-final/`: 50.135.040 bytes,
  SHA-256 `4585EDF9AE2A25066BF82667B76132D6605E2D3D07424E91DAA7D569844A1F92`.
  `eboot.bin`: 27.314.252 bytes, SHA-256
  `3F19825EB6A446826F62E008331FB1B64C72CBBE0AC8DB5F281A4BC15A10EFB8`.
- `param.json`, versión visible en RML y banner runtime coinciden en
  `02.000.052`. El gate del paquete pasó 15 comprobaciones, 0 avisos y 0
  fallos; MkPFS verificó la imagen con 0 avisos y 0 errores. No se ejecutaron
  pruebas ni se validó esta build en PS5.

## 02.000.050 — 2026-10-02 · interfaz CD y lógica de controles

- La propuesta CD se integra en una superficie superior que aparece solo cuando
  se detecta `cd1`. Se eliminan los botones SCAN/PREV/NEXT/STOP del panel superior;
  la franja inferior conserva su atlas `buttons.tga` de la build v049 (SHA-256
  `076B05D06DAFC1C23A9B39FC4023266AF0D22B15D1DF6ED126F34402C4D3904E`). El
  usuario indicó que los botones del último preview no son los canónicos; la
  correspondencia visual no queda validada aunque el atlas coincida.
- La animación pasa a un icono ámbar dentro de la pantalla LCD, con doce cuadros
  TGA independientes y carga diferida para `cd1`; no usa la tira brillante que
  desbordaba sobre la lista y el estado.
- La cruceta arriba/abajo alterna el foco entre las superficies; izquierda/derecha
  conserva el recorrido de los controles inferiores. Las acciones de CD usan los
  controles inferiores contextualizados y la selección del disco. Queda pendiente
  cotejar su resultado con los controles canónicos esperados y probarlo en PS5.
- Build `PPSA99001` compilada en
  `out/prospero-radio-02.000.050-ppsa-cd-controls/`: FFPFSC de 52.953.088 bytes,
  SHA-256 `E519F3DFA3C8F94D00666DF71BC179B3E972C37EF5165749FD7AB2656D0C264A`;
  `eboot.bin` SHA-256
  `BA1F3884E1708B8D665EE2E899BE981E26A46A4AFD7229FF124950BB1A69D886`.
  `param.json`, RML y banner indican `02.000.050`; el gate pasó 14 checks, 0
  avisos y 0 fallos. La build de consola aún no está validada por el usuario.

## 01.000.046 — 2026-10-01 · versión canónica de la build comprobada

- La versión canónica avanza a `01.000.046`. El usuario informa que se comporta
  como la última build que había revisado: conserva sus virtudes y sus defectos
  de DASH y algunos HLS.
- La fuente empaquetada obtiene y guarda la lista AUX en memoria a través de las
  operaciones RPC 9/10; el payload conserva el documento bajo `/data/radio`.
  La app no materializa `radio-aux.m3u` en `/download0`.
- El servidor y la app admiten la selección de M3U, M3U8, PLS, XSPF y ASX con
  límite de 4 MiB, y la UI implementa borrado de una fila AUX al mantener
  Cuadrado durante 3 segundos con cuenta atrás. Estos recorridos requieren
  pruebas específicas en hardware, registradas en pendientes.
- El paquete FFPFSC local mide 52.297.728 bytes y tiene SHA-256
  `26B77747F9142472B0213B33E14FFAD435CB3A6B3B06157CAAD0848ADE4B8940`.
  El gate de recursos pasó con 14 aprobados, 0 avisos y 0 fallos. El asset de
  la release publicada manualmente tiene el mismo hash.
- El usuario probó la v046 en PS5: mantiene una respuesta ágil, recupera la
  configuración previa de `/data/radio` y la cruceta arriba/abajo recorre la
  lista al mantener pulsado.
- En reproducción, el test DASH 06 ahora se oye como ruido blanco; el usuario
  no reporta un crash en esta prueba. Algunos streams HLS siguen fallando.
  El resultado auditivo no demuestra que DASH o HLS funcionen correctamente;
  el diagnóstico de protocolo/codec sigue abierto.
- No se deben confundir estos resultados con el informe anterior de v045, donde
  el test DASH 06 se reportó como crash. Ambas observaciones se conservan con
  su versión correspondiente en [`docs/PENDIENTES.md`](docs/PENDIENTES.md).

## 01.000.042 — 2026-09-28 · correcciones tras observación de la v039

- Baseline inicial: las capturas anteriores del usuario muestran `01.000.039`.
  La lista presentaba texto superpuesto, Ajustes aparecía sobre fondo blanco y AUX/M3U
  era difícil de descubrir. No atribuir esos síntomas iniciales a otra versión.
- La navegación RADIO/FAVORITOS/AUX ahora permite seleccionar AUX incluso si
  no hay emisoras cargadas; la pestaña visible y la fuente seleccionada siguen
  el mismo ciclo en ambos sentidos.
- La app persiste presets de listas externas como snapshots `radio_station_t`
  además de los presets de catálogo. Los chips P1/P2/P3 tienen reglas CSS con
  especificidad explícita para distinguir vacío, guardado, activo y confirmación.
- El helper `SetText` escapa `&`, `<` y `>` antes de pasarlos a `SetInnerRML`,
  para que nombres y metadatos de M3U no se interpreten como marcado.
- Los nombres de emisora M3U usan una identidad estable basada en URL y no en
  su posición dentro del fichero. Se mantiene el panel AUX del payload con una
  presentación de conectores traseros RCA/jack y carga de fichero `.m3u`.
- Se corrigió el contrato de retorno de `SavePresets()` para reflejar fallos
  de escritura/renombrado y sincronización con el payload.
- El atlas actual no declara frames P1/P2/P3: el estado visual se resuelve con
  CSS sobre el LCD, no se afirma una textura de preset que el manifiesto no
  contiene. La detección física del jack queda pendiente; la búsqueda dinámica
  es solo tentativa y necesita confirmación en consola.
- Se mantuvo EQ dentro del rectángulo de cristal y no se añadieron controles de
  ajustes/temas a la navegación principal. El color RGB del DualSense no se modificó.
- Artefacto: `out/prospero-radio-01.000.042/PPSA99001.ffpfsc`, 52,887,552
  bytes, SHA-256 `7C515C6EC3214429264E678B0E30391C370DA4F3E41048B863176A351051D3D7`.
  `eboot.bin` SHA-256 `A8ECA863E6870A7F5422530F39F4E5EF00BFB1E6DE32DF325004B58B548ED757`.
  `param.json`, RML y banner indican `01.000.042`; gate: 14 aprobados,
  0 warnings, 0 fallos. Las fotos del usuario del 28-09-2026 muestran la app en PS5
  con sello `01.000.042`, emisora y listas visibles. Persisten pestañas superpuestas/
  vacías, `JACK N/A`, contraste bajo en el encabezado web AUX y duplicación de M3U
  reportada al reenviar una lista. La prueba es parcial y el hash instalado no se cotejó.

## 01.000.041 — 2026-09-28 · importación M3U y lista AUX en RADIO

- La página del servidor AUX adopta los materiales visuales de la radio (madera,
  metal cepillado y cristal ámbar) y permite escoger un fichero `.m3u` desde el
  navegador móvil o de escritorio. Se conserva el pegado de texto como alternativa.
- La subida transmite el contenido original con `Content-Length`; el payload lo
  guarda en `/data/radio/radio-aux.m3u` usando temporal, `fsync` y renombrado.
- La app lee la lista importada y crea emisoras externas con título y metadatos
  `#EXTINF`, filtrando a URLs HTTP(S). Se mantiene separada del catálogo Radio
  Browser. La disponibilidad de cada URL se confirma al intentar reproducirla.
- Al abrir RADIO, la app recupera la lista AUX persistida; izquierda/derecha
  alternan entre Radio Browser, Favoritos y AUX. BARRIDO sigue permitiendo
  copiarla y abrirla directamente.
- No se cambió el color RGB del mando ni el atlas de texturas.
- Hubo un intento de compilación 039 que encontró un NUL en un literal
  generado. Aun así, las capturas del usuario identifican por pantalla una
  ejecución v039. No se halló en `out/` el paquete/hash exacto instalado, así
  que no se vincula ese intento fallido con la ejecución fotografiada.
- La 041 queda como candidata histórica; no hay artefacto 041 disponible en
  el `out/` actual ni una prueba de consola atribuible a esa versión. El
  artefacto actual es la 042, descrita arriba.

## 01.000.038 — 2026-09-28 · identidad estable de presets y refresco de EQ

- Los presets guardan el UUID de la emisora junto al índice de compatibilidad.
  La recuperación busca por UUID en el catálogo y ya no interpreta una posición
  de Favoritos como la misma posición de la lista general.
- Los ficheros antiguos con tres índices se leen y migran cuando la emisora
  puede resolverse en la vista general cargada. Un índice antiguo que proviniera
  de otra vista no conserva información suficiente para reconstruir con certeza
  su UUID.
- El lanzamiento de un preset pendiente tras detener la emisora anterior usa
  también el UUID. HOME conserva los metadatos seleccionados para no volver a
  mostrar `Tuning...` por un cambio de página o vista.
- El EQ sincroniza el nombre de preset desde el servicio y actualiza las cinco
  etiquetas al iniciar y cada vez que se abre la pantalla; ya no requiere mover
  el joystick para actualizar la vista. La edición personalizada inicia el ciclo
  siguiente en el preset Flat.
- El usuario confirmó que el color RGB del DualSense ahora es correcto. No se
  cambió la lógica de color en esta revisión.
- Artefacto: `out/prospero-radio-01.000.038/PPSA99001.ffpfsc` (52,887,552
  bytes; SHA-256
  `EA2023C7D294F6CEC063509AF783A22656D165BB43FCB5752290B8C7EF0D9C19`).
  Build FFPFSC terminada con salida 0; gate 14/14, 0 warnings, 0 fallos;
  empaquetador sin errores ni warnings. Sin prueba en PS5 para esta versión.

## 01.000.037 — 2026-09-28 · AUX bajo demanda y correcciones de estado

- AUX deja de arrancar por defecto. Al abrir su vista, la app solicita al payload
  `START_AUX`, consulta el estado y el payload enlaza TCP 7000. Al salir de AUX,
  apagar la radio o cerrar la app, la app solicita `STOP_AUX` para liberar el
  listener. Es la API del payload la que posee el servidor.
- El servidor recibe `POST /list` con `Content-Length` hasta 32 MiB y conserva la
  lista anterior si la carga queda incompleta. Escribe primero un temporal,
  sincroniza y renombra `/data/radio/radio-aux.m3u`. AUX no tiene autenticación;
  debe usarse solo en una LAN de confianza.
- BARRIDO solicita el fichero AUX por RPC (ID 6) y lo copia a
  `/download0/radio-aux.m3u`. La app cuenta líneas `http://`/`https://`; no
  importa todavía esas entradas al catálogo reproducible ni verifica que sus
  streams estén disponibles. La pantalla informa copia/conteo, no reproducción.
- Cambia el canal entre app y payload: elfldr recibe la solicitud del ELF con
  `pipe=0`; después la app se conecta al RPC del payload en `127.0.0.1:7001`.
  El payload acepta `STATUS`, `START_AUX`, `STOP_AUX` y `ATTACH` además de las
  operaciones de persistencia. El modo de carga, loopback y cierre siguen
  pendientes de comprobación en PS5.
- HOME prioriza los metadatos de la emisora realmente en reproducción y refresca
  al cambiar el índice activo; evita mostrar el estado de sintonía del cursor de
  lista cuando su índice no está en la página cargada.
- La señal de presets usa pulsos temporizados de 180 ms, color solicitado
  (255,128,0), un ciclo por zona P1/P2/P3 y estado fijo mientras siga activo el
  preset. `scePadSetLightBar` usa la estructura de cuatro bytes y registra los
  errores. El tono ámbar real debe confirmarse en hardware.
- No se añade detección de auriculares: no hay una API de conexión de audio
  verificada en este proyecto. El indicador no debe presentarse como detección
  física hasta implementar esa fuente de estado.
- Artefacto: `out/payload-bridge-01.000.037-api/PPSA99001.ffpfsc` (52,887,552
  bytes; SHA-256
  `E917678D0A682DF99C0EA86920158DA2945BF7549C6466815847B263DFA63C27`). Build
  con salida 0; gate del paquete 14/14, 0 warnings y 0 fallos; el empaquetador
  registró 0 errores y 0 warnings. Estos resultados verifican artefactos, no la
  ejecución de ELF, la red ni la presentación en consola.

## 01.000.036 — 2026-09-28 · puente persistente integrado y controles corregidos

- Promueve el puente de `/data/radio` desde sonda de diagnóstico a la build normal
  `ffpfsc`; el payload se nombra `ProsperoRadioDataBridge.elf`, se compila con
  warnings como errores y se entrega automáticamente a elfldr al iniciar.
- Sustituye el listener auxiliar TCP 7001 por el canal heredado `pipe=1` de
  elfldr: la app y el payload comparten el socket aceptado en stdin/stdout. Se
  elimina una dependencia innecesaria de `SceNet` del ELF de persistencia.
- Al salir, guarda el estado, manda `STOP`, hace half-close y espera EOF. Si el
  ACK no llega, el cierre de entrada también hace que el bucle del payload lea
  EOF y retorne. La liberación real de su proceso todavía necesita log/klog de PS5.
- Reintenta la conexión a elfldr durante un segundo si el listener local aún no
  está listo; si el bridge no arranca, la radio conserva el fallback de `/download0`.
- Arranca AUX después del servicio de radio, para que el importador de listas
  escuche en el puerto 7000 sin tener que entrar primero en esa vista.
- Lee los contactos DualSense desde `ScePadData.touchData.touch[0]` (conteo
  `0x34`, coordenadas `0x3c/0x3e`, ID `0x40`), actualiza la antigüedad solo con
  un contacto válido y pasa un `s_SceLightBar` de 3 bytes. La zona se fija al
  click; contacto sin click no actúa; el click largo conserva 3 segundos.
- Corrige el número de pulsos de la barra: cada preset emite 1, 2 o 3 ciclos,
  no el doble. AUX y BARRIDO conservan acciones distintas y en el orden actual.
- Mantiene la persistencia de catálogo/favoritos/EQ/presets y la corrección de
  índices globales para páginas. Los fixes anteriores de EQ, reproducción y
  paginado fueron observados por el usuario en consola; aún hay que repetir la
  prueba con el paquete 036.
- Motivo: v034 probó acceso de payload a `/data/radio`, pero la app rechazó el
  loopback 7001. Reutilizar el socket que elfldr documenta para `pipe=1` elimina
  ese punto de fallo y deja 7000 exclusivamente a AUX.
- Se quitó una constante HTTP sin uso que quedó tras sustituir el servidor AUX;
  se corrigió en el código generado, sin ocultar warnings ni retirar funciones.
- Artefacto compilado: `out/payload-bridge-01.000.036-final/PPSA99001.ffpfsc`
  (52,887,552 bytes; SHA-256
  `150697e6f6d57ef315efbddf625a30c85b2b1063d87666d8fc93ee81387a0227`). El
  contenedor reporta 0 errores/0 warnings y el gate marca 14 passed, 0 warnings,
  0 failures. La prueba de hardware sigue pendiente.

## 01.000.035 — estado fuente previo, sin FFPFSC confirmada

- Incorporó el primer cableado de persistencia por socket local y los cambios de
  UI/controladores en el árbol compartido. No se ha localizado un paquete 035
  verificable en `out/`; no atribuirle resultados de hardware.

## 01.000.034 — 2026-09-28 · zonas táctiles DualSense

- El click del panel se procesa por transición real de press/release y no se
  confunde con otros botones del mando.
- Cada evento conserva las coordenadas de contacto del mismo informe; P1/P2/P3
  se eligen por tercios horizontales (0–639, 640–1279, 1280–1919).
- Toque sin click se ignora. Click corto recupera; mantener click y contacto
  durante 3 segundos guarda. Levantar antes cancela.
- Se descartan coordenadas obsoletas y se registra cada gesto en el log.
- Requiere prueba en PS5: offsets táctiles y respuesta física aún no verificados.

## 01.000.033 — 2026-09-28 · puente de persistencia en loopback

- Sustituye la sonda HTTP de una petición por `ProsperoRadioDataBridge.elf`,
  con protocolo binario acotado en `127.0.0.1:7001`; reserva el puerto 7000 para AUX.
- Restaura antes de iniciar el servicio el catálogo, favoritos, EQ y presets
  desde `/data/radio`, y siembra los ficheros ausentes desde la caché de `/download0`.
- Persiste cambios pequeños inmediatamente y sube snapshots SQLite consistentes
  después de la sincronización completa; los `PUT` se publican por rename atómico.
- Corrige la resolución de índices globales en páginas y favoritos, mantiene el
  orden/filtro activos y hace que el UUID seleccionado controle la acción.
- Build diagnóstico pendiente de validar en PS5, incluidos AUX, reinicios y cierre.

## 01.000.032 — 2026-09-28 · informe del payload en `/data/radio`

- El payload crea y trunca su propio `prospero-payload-probe.log` dentro de
  `/data/radio`; cada etapa se guarda allí y en klog.
- Tras la respuesta HTTP, la app abre ese fichero y comprueba los marcadores
  de inicio y de lectura/escritura del directorio compartido.
- El informe de la app sigue en `/download0`; el build de consola debe mostrar
  `shared payload log read PASS` para demostrar lectura entre procesos.

## 01.000.031 — 2026-09-28 · conexión local compatible con PS5

- Conserva el inicio diferido de la sonda y reduce la comprobación de salud a
  ocho intentos separados por 50 ms.
- Restaura la conexión TCP normal a elfldr: la consola rechazó el `connect`
  no bloqueante de la 030 segura con `errno=13`.
- La telemetría 01.000.030 confirma ejecución del ELF, acceso de escritura a
  `/data/radio` y respuesta del listener local en el puerto 7000.
- La sonda no puede escribir directamente en el `download0` de la app; su
  resultado resumido queda registrado por la app y el detalle, en klog.

## 01.000.028 — 2026-09-27 · sondeo optativo de payload local

- Añade `probe-ffpfsc`, una variante diagnóstica separada de las builds normales.
- Al iniciar, la app intenta transferir un ELF mínimo al elfldr local en el puerto
  9021; registra cada etapa en `/download0/prospero-payload-probe.log`.
- El payload informa por klog y comprueba `/data/radio`, escritura temporal en
  `/download0` y una respuesta local de una sola petición en el puerto 7000.
- No termina la app, no deja un servicio persistente y no demuestra todavía el
  acceso desde LAN ni la ejecución en hardware.

## 01.000.030 — 2026-09-27 · corrección de ruta de la sonda

- Usa el contenido montado de ShadowMountPlus en `/system_ex/app/PPSA99001`.
- El klog 01.000.029 mostró que elfldr no encontraba la URI anterior bajo
  `/user/app`; el acceso desde el proceso de elfldr aún requiere validación.
- El bloqueo observado corresponde al watchdog de `SceShellUI`; el klog no
  registra un crash del proceso `PPSA99001`.

## 01.000.029 — 2026-09-27 · primera URI local de elfldr

- La app conectó al puerto 9021 y envió una URI `file:` con nombre de ELF.
- elfldr rechazó la ruta bajo `/user/app`; no hay evidencia de ejecución del
  payload en esta versión.

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

La evidencia de cada versión y sus límites se conservan en esta cronología y en
[`docs/PENDIENTES.md`](docs/PENDIENTES.md).

## 01.000.027 — 2026-09-27 · registro histórico

- La fuente declara esta versión en `overlay/apply-vulkan.py` y
  `sce_sys/param.json`.
- El autor confirma en consola la edición/persistencia del EQ, una reproducción más
  fiable y navegación por el catálogo completo.
- Continúan pendientes el mapeo del panel táctil, el servidor AUX, el indicador de
  auriculares y la reconstrucción de Ajustes/temas.
- La versión mostrada por esas pruebas no se cotejó con aquel árbol; los resultados son
  observaciones manuales, no una certificación de esa versión fuente.
- El build 028 registrado no superó el gate del paquete. No se creó una release a
  partir de ese resultado.

---

## 01.000.019 — 2026-09-26 · 🔁 frontend rehecho como radio física

**Rediseño completo del frontend**: la lógica antigua (grid de tarjetas, pestañas,
paginación, pie de mandos) desaparece; la interfaz se comporta como la radio que
imita.

- **Los 7 botones físicos SON la navegación**: cruceta ←→ mueve el "dedo" por los
  botones del frontal (estados focus/selected/pressed del atlas de Luna) y ✕ los
  pulsa: Home, Radio, Favoritos, Géneros, Buscar, Ajustes, Play/Pausa.
- **Dial derecho = sintonizar**: girar recorre el catálogo entero y sintoniza al
  momento si estaba sonando (como una radio de verdad); las flechas ⏮/⏭ del atlas se
  iluminan en la dirección del giro.
- **Dial izquierdo = volumen** con curva perceptual.
- **Listas dentro del cristal**: Radio/Favoritos muestran 7 filas navegables con el
  dial (✕ sintoniza la fila, □ favorito, ←/→ cambia de lista); Géneros lista los
  géneros y ✕ filtra.
- **Cristal único**: Home muestra emisora sintonizada, códec/bitrate, estado de
  reproducción y ecualizador animado, con la paleta LED del manifiesto.
- `radio_app.cpp/.hpp` se sustituyen enteros desde el overlay (adiós a los parches
  frágiles sobre la capa de UI antigua).

## 01.000.018 — 2026-09-26 · 🎛️ interfaz integrada + control rotatorio

- **Frontal híbrido 4K**: el fondo pasa a `radio_front_hybrid_4k.ktx2` (fachada de
  radio sobre la fotografía cálida, composición de Luna) con la geometría exacta del
  manifiesto híbrido.
- **Atlas de controles en marcha** (sprites de Luna): el dial de volumen muestra el
  arco/aguja según el nivel real (21 frames, `volumen/5` redondeado), las flechas
  ⏮/⏭ del sintonizador se iluminan en ámbar en la dirección del último salto de
  emisora, y los siete botones físicos del frontal reflejan la vista activa.
- **Volumen perceptual**: curva audio-taper (potencia 2.5) en `sceAudioOut` — el
  recorrido completo del mando se oye de forma pareja (antes 50→100 % era casi plano).
- **Potenciómetro rotatorio en los sticks**: girar el stick izquierdo (movimiento
  circular) sube/baja volumen — vuelta completa ≈ 50 % — y el derecho salta emisoras
  con el dial iluminándose en la dirección del giro. Zona muerta por radio y límite de
  velocidad por paso; adiós al sube/baja vertical.
- **Selector de acabado en Ajustes**: nueva fila «Cabinet Finish» (Walnut / Silver /
  Graphite) con persistencia en `/download0/radio-theme.txt`. Walnut usa el híbrido 4K
  con el atlas activo; Silver y Graphite usan sus frontales planos (cargados desde TGA
  de 24 bpp, ahora soportados por el loader con orden de filas correcto).
- **Ajustes dentro del cristal**: el panel se re-estila con la paleta LED del
  manifiesto (#F4BE76 / #E2A658 / #BC7E39).

**Pendiente / conocido:**

- Descuadres de texto restantes en el catálogo (posiciones del grid sobre el frontal
  híbrido).
- Aro de foco del volumen aún sin activar (pendiente de cablear al gesto).
- Variantes híbridas de Silver/Graphite para que el atlas alinee en todos los temas.

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
  crece; las notas históricas restantes están recogidas en esta cronología.

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
| `src/`, `include/`, `tooling/`, `vendor/` | Código base y dependencias del fork |
| `docs/` | Compilación, arquitectura, validación y pendientes actuales |
| `out/` | Paquetes, logs y volcados locales; ignorados por Git |

## Distribución

Los paquetes (`PPSA99001.ffpkg` / `PPSA99001.ffpfsc`) no se versionan en Git por
tamaño. La versión v042 y los SHA-256 del artefacto local están registrados al inicio
de este changelog; una publicación binaria requiere un Release explícito.

## 01.000.026 (2026-09-26)
- Nueva interfaz física: rótulos POWER/BANDA/MEM/AUX/BARRIDO/EQ/PLAY y estados de foco/selección con contraste real (atlas y frontal regenerados).
- POWER = salida limpia exit(0). BANDA = lista de emisoras. MEM = favoritos. AUX/BARRIDO/EQ: superficies visibles (funciones de fondo en v027).
- Fuera Silver/Graphite y cambio de tema. Fix: el log de runtime ya no nace vacío (stderr sin buffer).
- build_test: POWER apaga con fade del LCD y salida cruda (_Exit), sin crash de teardown.

## 01.000.026 build_test 2 (2026-09-27) — prototipo, no release

- Se integraron cinco bandas de EQ, presets y persistencia; la edición y persistencia
  quedaron confirmadas por el autor en una prueba posterior.
- El panel táctil y el servidor AUX se añadieron como prototipos, pero las pruebas
  actuales indican que el mapeo táctil es incorrecto y que el servidor no queda
  disponible. No describir estas funciones como completas.
- Los pendientes de consola y los criterios de aceptación están en
  [`docs/PENDIENTES.md`](docs/PENDIENTES.md).
