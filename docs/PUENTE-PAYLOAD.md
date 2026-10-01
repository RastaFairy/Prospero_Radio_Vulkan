# Puente de persistencia y servidor AUX

El paquete normal incluye `ProsperoRadioDataBridge.elf`. La app solicita su carga
mediante elfldr y usa una API local para persistencia y ciclo de vida de AUX. El
servidor web de listas escucha en TCP 7000 mientras la superficie AUX está activa.

## Inicio y canal de control

`RadioApp::Initialize` intenta abrir el ELF empaquetado, solicita a elfldr su
carga mediante `127.0.0.1:9021` y envía `pipe=0` con el PID de la app como
argumento. Después, la app abre un canal RPC TCP separado con el payload en
`127.0.0.1:7001`. `pipe=0` no es el canal de órdenes app-payload: elfldr recibe
la petición de carga y el RPC local se establece por separado.

El protocolo `PRPC` v1 usa cabecera de 12 bytes y operaciones acotadas:

- `1 PING`, `2 GET`, `3 PUT`, `4 STOP`, `5 STATUS`.
- `6 START_AUX` enlaza el servidor externo en el puerto 7000.
- `7 STOP_AUX` cierra el listener y `8 ATTACH` actualiza el PID propietario.
- `9 GET_AUX_BUFFER` lee la lista persistente directamente a memoria de la app;
  `10 PUT_AUX_BUFFER` guarda sus bytes desde memoria en el payload.
- IDs de archivo: `1` catálogo SQLite, `2` favoritos, `3` EQ, `4` presets,
  `5` informe compartido, `6` `/data/radio/radio-aux.m3u` y `7` favoritos AUX.

La app consulta el estado AUX y solo lo marca activo si responde la API. Si el
payload no arranca, no hay `/data/radio` escribible o falla el RPC, el registro
de app queda en `/download0/prospero-payload-probe.log`; la función dependiente
de `/data` no queda demostrada por el mero empaquetado del ELF. El payload deja
su informe en `/data/radio/prospero-payload-probe.log`.

## Ciclo AUX y flujo de una lista

1. Al entrar en la superficie AUX, la app llama a `START_AUX`; el payload enlaza
   TCP 7000 en todas las interfaces. Si el enlace falla, devuelve error y la
   pantalla no debe indicar que está listo.
2. Un equipo de la misma LAN abre `http://IP-DE-LA-CONSOLA:7000/` y envía la
   lista con `POST /list`, `Content-Length` y el tipo de lista indicado en
   `X-Playlist-Format`.
3. El payload limita el cuerpo a 4 MiB y escribe
   `/data/radio/radio-aux.m3u.tmp`; exige recibir el tamaño completo, hace
   `fsync` y renombra el fichero. Una carga incompleta conserva la lista previa.
4. La lista recibida se publica en `/data/radio/radio-aux.m3u` mediante archivo
   temporal, `fsync` y renombrado. Un sobre de 9 bytes conserva el formato aunque
   el nombre persistente del archivo termine en `.m3u`.
5. Al elegir BARRIDO, la app recupera el contenido por `GET_AUX_BUFFER` (RPC 9)
   a un buffer en memoria, analiza las entradas y presenta emisoras AUX. Los
   metadatos se conservan y solo se aceptan URLs HTTP(S); no se crea una copia
   de la lista en `/download0`.
6. La importación no prueba por adelantado que todos los streams estén vivos. La
   conexión real se determina al sintonizar. El payload reemplaza la lista
   anterior al recibir otra carga; el escáner de la app descarta URLs repetidas
   dentro del M3U recibido (incluido en la fuente compilada para v046). Si la
   repetición entre cargas persiste, cotejar el M3U recibido y la pantalla para
   localizar qué capa reintroduce las filas.
7. Al editar o borrar una emisora, la app serializa la lista en memoria y la
   guarda mediante `PUT_AUX_BUFFER` (RPC 10), directamente en `/data/radio`.
8. Al salir de AUX, usar POWER o cerrar la app, se solicita `STOP_AUX`. En el
   cierre normal también se guardan los datos persistentes, se manda `STOP` y
   la app espera EOF del canal del payload hasta el límite implementado.

El servidor HTTP no tiene autenticación y escucha en la red local: usar solo en
una LAN de confianza. El payload atiende cada cliente AUX en su bucle principal;
una conexión HTTP lenta puede retrasar temporalmente otras órdenes, incluido
`STOP_AUX`. Es un riesgo de diseño que debe medirse en consola.

## Persistencia

El payload crea `/data/radio` si el sistema ya montó `/data` y concede acceso.
No monta `/data` ni carga exploits. El app sandbox usa `/download0` como copia de
trabajo; el payload es quien lee y escribe:

- `radio-browser.sqlite3` — catálogo.
- `radio-browser-favorites.bin` — UUID de favoritos.
- `radio-eq.txt` — formato `EQ2`: 12 ganancias de banda y faders L/R; la app
  migra el formato anterior de cinco valores. Está incluido en la fuente v046;
  no inferir una verificación manual de cada banda a partir de la prueba general.
- `radio-presets.bin` — tres memorias del panel táctil.
- `radio-aux.m3u` — lista AUX recibida/editada; los primeros 9 bytes guardan
  el identificador de formato (`M3U`, `M3U8`, `PLS`, `XSPF`, `ASX` o auto).
- `radio-aux-favorites.bin` — favoritos de la lista AUX.
- `prospero-payload-probe.log` — evidencia de acceso y actividad del payload.

Las copias persistentes se publican por temporal, `fsync` y rename. Los límites,
permisos reales, la persistencia tras reiniciar y la finalización del proceso
requieren evidencia de runtime en PS5.

## Estado observado en 01.000.042

El build local v042 contiene el ELF nombrado y superó el gate del paquete. Las
capturas de consola del usuario muestran `01.000.042`, una emisora y las listas
AUX en pantalla; también muestran pestañas defectuosas y `JACK N/A`. El paquete
instalado no se cotejó por hash. Estas capturas no demuestran por sí solas el
ciclo completo de escritura, persistencia tras reinicio ni cierre del payload.

## Estado observado en 01.000.046

El usuario confirma que la app reconoce su configuración previa en `/data/radio`.
Eso acredita que la configuración ya presente fue leída en esa ejecución, pero
no identifica cada archivo ni prueba por sí solo una escritura AUX nueva o una
reapertura tras reiniciar. La lista AUX usa buffers RPC en v046; los favoritos
AUX y otros ficheros de trabajo mantienen rutas locales independientes.

## Verificación pendiente en hardware

En la consola, contrastar el klog con
`/download0/prospero-payload-probe.log` y el informe bajo `/data/radio`:

- inicio por elfldr, handshake en 7001, operaciones de estado/start/stop y PID;
- GET/PUT de ficheros y persistencia tras cerrar y volver a abrir la app;
- acceso HTTP al 7000 desde otro dispositivo, rechazo de longitud ausente,
  truncada o superior a 4 MiB y conservación de la lista anterior;
- BARRIDO vuelve a leer la lista persistente, carga emisoras/metadatos y muestra
  una sola fila por URL repetida; validar también importación M3U8, PLS, XSPF y
  ASX con codificaciones, metadatos y URLs relativos pertinentes;
- confirmar que fetch, edición y guardado mantienen la lista en memoria de app y
  en `/data/radio`, sin crear `/download0/radio-aux.m3u`;
- mantener Cuadrado 3 segundos sobre una fila AUX, confirmar cuenta atrás,
  cancelar al soltar antes y comprobar que al completar se elimina solo esa fila
  y queda persistida en `/data/radio`;
- dejar un cliente lento conectado y comprobar que AUX se detiene al abandonar
  la vista y que POWER/cierre del sistema liberan tanto listener como payload.

La importación de M3U al catálogo, su reproducción real y el color físico de la
barra luminosa siguen siendo comprobaciones separadas. La detección del jack del
DualSense queda fuera del alcance del proyecto; `JACK N/A` no se considera una
lectura física validada.
