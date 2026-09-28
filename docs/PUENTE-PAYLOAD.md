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
   lista con `POST /list` y `Content-Length`.
3. El payload limita el cuerpo a 32 MiB y escribe
   `/data/radio/radio-aux.m3u.tmp`; exige recibir el tamaño completo, hace
   `fsync` y renombra el fichero. Una carga incompleta conserva la lista previa.
4. Al elegir BARRIDO, la app recupera por RPC el ID 6 desde `/data/radio` hacia
   `/download0/radio-aux.m3u`, analiza las entradas M3U y presenta las emisoras en
   la fuente AUX. Los metadatos `#EXTINF` se usan como nombre/grupo; solo se
   aceptan URLs HTTP(S).
5. La importación no prueba por adelantado que todos los streams estén vivos. La
   conexión real se determina al sintonizar. **La deduplicación al reenviar una
   lista idéntica está pendiente**: el usuario observó que una nueva carga puede
   añadir repetidas las emisoras ya importadas.
6. Al salir de AUX, usar POWER o cerrar la app, se solicita `STOP_AUX`. En el
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
- `radio-eq.txt` — ganancias de cinco bandas.
- `radio-presets.bin` — tres memorias del panel táctil.
- `radio-aux.m3u` — lista recibida por AUX.
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

## Verificación pendiente en hardware

En la consola, contrastar el klog con
`/download0/prospero-payload-probe.log` y el informe bajo `/data/radio`:

- inicio por elfldr, handshake en 7001, operaciones de estado/start/stop y PID;
- GET/PUT de ficheros y persistencia tras cerrar y volver a abrir la app;
- acceso HTTP al 7000 desde otro dispositivo, rechazo de longitud ausente,
  truncada o superior a 32 MiB y conservación de la lista anterior;
- BARRIDO vuelve a leer la lista persistente, carga las emisoras con sus metadatos
  y no agrega otra copia de una URL ya importada (esta deduplicación sigue pendiente);
- dejar un cliente lento conectado y comprobar que AUX se detiene al abandonar
  la vista y que POWER/cierre del sistema liberan tanto listener como payload.

La importación de M3U al catálogo, su reproducción real, la detección de
auriculares y el color físico de la barra luminosa siguen siendo comprobaciones
separadas.
