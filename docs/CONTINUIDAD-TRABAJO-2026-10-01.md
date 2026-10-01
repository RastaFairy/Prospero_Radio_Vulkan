# Continuidad de Prospero Radio — 2026-10-01

## Estado del checkout

- Proyecto: `D:\prospero_modern`, rama `main`, HEAD `3d99252` en la última inspección.
- El árbol compartido está muy modificado y contiene cambios de usuario/GLM y artefactos sin seguimiento. Antes de tocar nada, vuelve a ejecutar `git status --short --branch` y revisa el diff. No restaures, limpies, guardes en stash ni sobrescribas nada ajeno.
- La build `01.000.046` se compiló el 01-10-2026, pasó el gate de recursos (14 aprobados, 0 avisos, 0 fallos) y su FFPFSC local coincide por SHA-256 con el asset de la release publicada manualmente. El usuario confirmó que probó esa versión en PS5.
- El usuario informa que la app va ágil, reconoce la configuración previa en `/data/radio` y que mantener arriba/abajo recorre la lista. En DASH test 06 ahora se oye ruido blanco y no se reporta crash en esa prueba; algunos HLS siguen fallando. Esto no es prueba de reproducción correcta.
- El usuario publicó manualmente la release/tag `01.000.046`; mantenerla intacta. En esta ronda se autorizó actualizar el repositorio y la documentación. No crear ramas/worktrees, no recompilar ni ejecutar pruebas sin nueva orden.

## Objetivo activo que queda por terminar

El usuario pidió para la siguiente build:

1. Diagnóstico de conexión/reproducción que indique fase y error de cada emisora, con límites para que los logs no saturen almacenamiento/memoria.
2. Validar en hardware la importación de M3U/M3U8, PLS, XSPF y ASX, incluida la detección de tipo/contenido.
3. Validar en hardware que mantener Cuadrado 3 s borra solo la emisora seleccionada, muestra cuenta atrás y cancela al soltar antes.
4. Corregir el audio DASH AAC del test 06; no declarar compatibilidad mientras dé ruido blanco. Investigar también los fallos HLS.
5. La transferencia de la lista AUX en memoria a `/data/radio` está implementada en v046; validar importación, edición, relectura y persistencia en PS5.

La conversación se ha detenido para transferir el contexto a otro chat. Los puntos 1–4 siguen incompletos/no validados.

## Hechos y límites de arquitectura

- En `overlay/payloads/prospero_radio_data_bridge.c`, `DATA_DIRECTORY` es `/data/radio` y la lista AUX se guarda como `/data/radio/radio-aux.m3u` mediante temporal + `rename`. El servidor acepta cuerpos de hasta 4 MiB más el sobre interno de formato.
- Desde v046, las operaciones RPC `GET_AUX_BUFFER`/`PUT_AUX_BUFFER` transfieren el documento AUX entre la app y el payload en memoria. La app parsea y edita desde su buffer y el payload persiste en `/data/radio/radio-aux.m3u` mediante temporal y `rename`; se deshabilitó la ruta local genérica para el ID de archivo AUX. El sobre de 9 bytes conserva el identificador de formato aunque el nombre sea fijo. La prueba de consola por caso sigue pendiente.
- El servidor HTTP y el puente/parser limitan el documento a 4 MiB más el sobre interno; el límite coincide en ambos lados. La petición lleva el tipo en `X-Playlist-Format`, y el parser también reconoce formatos por contenido. El test de codificaciones y metadatos continúa pendiente.
- Los favoritos AUX siguen teniendo un archivo de trabajo separado en `/download0/radio-aux-favorites.bin`; no confundirlo con una copia de la lista AUX. Catálogo SQLite y otras preferencias conservan copias locales de trabajo.
- No prometer que todo el proyecto ya es independiente de `/download0`: catálogo SQLite y preferencias siguen usando rutas locales de trabajo porque el sandbox de la app no puede abrir `/data/radio` directamente. Para eliminar también esas copias habría que trasladar esas operaciones al payload o diseñar otra interfaz RPC/VFS; es un cambio arquitectónico mayor.
- Excepción solicitada por el usuario: el log de diagnóstico de la app sigue en `/download0/prospero-radio.log` para que pueda copiarlo. `overlay/src/app_cpp_runtime.cpp` lo limita/rota a 2 MiB, comprobando mantenimiento cada 30 frames. No convertirlo en un log ilimitado ni registrar cada fragmento.

## DASH: evidencia separada por versión

- Con la build `01.000.045`, test 06 (`https://livesim.dashif.org/vod/WAVE/av/combined.mpd`) se reportó como crash de la app y error del sistema.
- Con la build `01.000.046`, el usuario informa ruido blanco y no reporta crash en esa ejecución. El audio sigue siendo incorrecto y DASH permanece sin resolver.
- Evidencia local disponible: `D:\prospero_modern\out\prospero-radio-01.000.045\TEST 06 DASH AAC-LC 8s DASH-IF .txt` (194510 bytes, fecha 2026-10-01 02:08 según la última inspección), más `download0/`, `radio/` y el paquete `PPSA99001.ffpfsc` en ese directorio.
- Lista de prueba proporcionada por el usuario: `C:\PS5\Garlic Manager\PS5\DEC\radio.m3u`.
- El código de v046 tiene diagnóstico parcial `mode=dash` para lectura de segmentos y comienzo/resultado de conversión, limitado a tres fragmentos. Eso solo aporta instrumentación; **no es un arreglo de DASH** ni está contrastado con un nuevo klog.
- El código actual asigna buffers grandes para MPD/init/segmento/output. Investigar límites, estados de fallo y liberación, y usar los datos de `TEST 06`/klog para aislar la fase. No afirmar causa sin evidencia. DASH no se debe declarar compatible por el mero hecho de que exista un parser.
- La firma actual de `hls_reader_open()` y su llamador en `src/radio_service.cpp` usan tres argumentos; la inconsistencia señalada en una inspección anterior ya no aparece en el árbol actual. Esto es comprobación estática, no evidencia de reproducción HLS correcta.

## Cambios incluidos en v046 y comprobaciones específicas aún abiertas

- `overlay/src/radio_app.cpp`: parser AUX para M3U, M3U8, PLS, XSPF y ASX; filtro de URLs duplicadas, lectura/edición mediante buffer, y gesto de borrado de Cuadrado por 3 s con cuenta atrás. Incluido en v046; falta validar los casos de uso concretos en PS5.
- `overlay/include/radio_app.hpp`: declaraciones/estado de esas funciones, incluido `aux_list_sync_retry_at_` y `SyncPendingAuxPlaylist()`.
- `overlay/src/payload_probe.cpp` / `overlay/include/payload_probe.hpp`: las operaciones RPC 9/10 leen/escriben AUX desde buffers; el formato se conserva en el sobre y el tamaño máximo es 4 MiB.
- `src/radio_playlist.cpp` / `include/radio_playlist.hpp`: soporte de parser complementario; el escaneo de AUX se realiza en `RadioApp`.
- `src/radio_service.cpp`: logs de reproducción y DASH; hay cambios amplios previos, así que inspeccionar diff antes de editar.
- `overlay/payloads/prospero_radio_data_bridge.c`: guarda el archivo de lista en `/data/radio`, limita cuerpos a 4 MiB y atiende GET/PUT de buffers. `radio_service_aux_stations_set()` recibe el conteo parseado de la app; no hay lectura de `aux_count_m3u()` desde `/download0` en la generación actual.
- El usuario pidió previamente mantener los logs acotados y no compilar sin orden explícita.

## Evidencia de reproducción reportada por el usuario (v045)

| Caso | Resultado en consola |
|---|---|
| MP3 MuyBuena, URL directa | Reproduce publicidad y luego queda en silencio |
| MP3 MuyBuena, ruta `/radio/8010/` | Reproduce publicidad y luego queda en silencio |
| HLS Radio Italia | No funciona |
| HLS RNE 1 | Funciona |
| HLS AES-128 Oceans | No funciona |
| DASH AAC-LC test 06 | Crash de app/sistema |

Estos son resultados de hardware reportados por el usuario; no explican por sí solos la causa del fallo.

## Restricciones de producto que siguen vigentes

- No reintroducir menús de ajustes ni temas visuales.
- Auriculares/jack se descartaron por coste; no retomar esa investigación.
- Mantener la lógica de mando/touchpad ya probada y evitar regresiones.
- Separar siempre fuente, salida generada, paquete, klog y observación de PS5. No dar una reparación por validada sin evidencia.

## Próxima secuencia recomendada

1. Leer este archivo y ejecutar el inventario actual de cambios; conservar todo el trabajo compartido.
2. Verificar en PS5 importación M3U/M3U8, PLS, XSPF y ASX, reimportación, edición por Cuadrado, persistencia en `/data/radio` y ausencia de archivo de lista AUX en `/download0`.
3. Seguir DASH de punta a punta y añadir un fix basado en evidencia nueva del test 06; los logs muestreados deben mostrar fase y resultado sin URL completa. Investigar los fallos HLS en ejecuciones atribuibles.
4. Verificar log rotado y respuesta en una sesión prolongada, conservando juntos el log exportable y klog.
5. Sin compilar ni probar hasta una orden explícita del usuario. Después de esa orden, seguir `CONTRIBUTING.md`, `docs/TESTING.md` y `AGENTS.md`, e informar por separado compilación, paquete y validación de hardware.
