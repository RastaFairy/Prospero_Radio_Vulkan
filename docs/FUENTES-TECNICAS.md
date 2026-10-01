# Índice de fuentes técnicas

Revisión de los repositorios públicos de los cinco perfiles indicados: 30-09-2026.
Este índice relaciona cada fuente con una parte concreta de Prospero Radio. Los
enlaces sirven para consulta; por sí solos no significan que su código forme
parte de esta aplicación ni que una función esté validada en nuestra consola.

Perfiles revisados: [BlackBearReloaded](https://github.com/blackbearreloaded),
[ArkSama](https://github.com/ArkSama), [Drakmor](https://github.com/drakmor),
[Mihawk-99](https://github.com/mihawk-99) y
[MexrlDev](https://github.com/MexrlDev).

Antes de adaptar código, revisar la revisión exacta, la licencia y sus
dependencias, y contrastar el comportamiento en el entorno nativo de Prospero
Radio. Las ramas `main` pueden cambiar después de esta revisión.

## Base que ya usa el proyecto

- [ProsperoRadio de BlackBearReloaded](https://github.com/blackbearreloaded/ProsperoRadio)
  — proyecto original del que deriva esta aplicación. Fuente para comparar el
  comportamiento heredado; el fork actual mantiene cambios propios.
- [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
  — base de la aplicación nativa, runtime y herramientas de construcción.
- [PS5_Vulkan de Mihawk-99, revisión fijada](https://github.com/mihawk-99/PS5_Vulkan/tree/085aac6a9e42c0d6337660e7148eb8990604052a)
  — base de la ruta gráfica. El script
  [`setup-ps5-vulkan.sh`](../overlay/setup-ps5-vulkan.sh) fija ese commit al
  preparar la fuente; el enlace a la rama actual no sustituye esa revisión.

## Referencias para correcciones y ampliaciones

- [PS5 Native Gamepad Input Research](https://github.com/blackbearreloaded/ps5-native-gamepad-input-research)
  — contrato de lectura por `libScePad`, datos de contacto del panel táctil,
  conexiones, transiciones y ciclo de vida. Prioridad para revisar el manejo
  del mando. La detección del jack queda fuera del alcance actual del proyecto.
- [PS5 Audio Decoding Research](https://github.com/blackbearreloaded/ps5-audio-decoding-research)
  — referencia para decodificación, AudioOut y rutas de reproducción. Sirve
  para investigar el motor de audio; sus pruebas de códecs no forman parte de
  una investigación de detección del jack, que está fuera del alcance.
- [PS5_RetroArch de Mihawk-99](https://github.com/mihawk-99/PS5_RetroArch)
  — aplicación nativa con entrada, audio estéreo, persistencia y pruebas de
  estabilidad documentadas; su README registra, entre otros casos, una sesión
  de 10 minutos con cambios repetidos de menú. Puede orientar el análisis de
  sesiones largas y cierre ordenado, pero esa duración no sustituye la prueba
  de varias horas reportada para Prospero Radio. Su evidencia pertenece a
  RetroArch y a sus ejecuciones.
- [ShadowMountPlus de Drakmor](https://github.com/drakmor/ShadowMountPlus)
  — referencia de payload con configuración y registros en `/data`, además de
  API HTTP/JSON configurable. Su documentación declara `127.0.0.1:10101` como
  dirección y puerto predeterminados; para aceptar conexiones de red cambia la
  dirección de escucha. Útil al comparar el servidor AUX y su ciclo de vida;
  su API y permisos no son intercambiables con los del puente de Prospero.
- [klogsrv de Drakmor](https://github.com/drakmor/klogsrv) — herramienta externa
  para recibir `/dev/klog` por socket durante diagnósticos. Es una referencia
  del flujo de captura de sistema, no una política para aumentar los logs
  internos de la aplicación.
- [PS5-Lapy-JB-Daemon de ArkSama](https://github.com/ArkSama/PS5-Lapy-JB-Daemon)
  — referencia acotada al canal de señal por fichero compartido entre app y
  payload. Su payload también modifica credenciales del proceso y emula una
  API de jailbreak; esas funciones y requisitos quedan fuera del alcance de
  Prospero Radio.
## Consultados y no seleccionados para esta ronda

- [AudioC0re de MexrlDev](https://github.com/MexrlDev/AudioC0re) es un reproductor
  bajo LuaC0re; no se selecciona para el trabajo actual.
- [FlappyC0re de MexrlDev](https://github.com/MexrlDev/FlappyC0re) anuncia un
  cambio automático de audio a auriculares, pero esa línea queda descartada al
  quedar la detección del jack fuera del alcance del proyecto.
- [ToolB0x de MexrlDev](https://github.com/MexrlDev/ToolB0x) se centra en
  funciones del mando que no están en el alcance actual. No se incorpora
  vibración ni sonido de interfaz en esta ronda.

Los repositorios restantes consultados no mostraron una relación concreta con
el catálogo, la interfaz, el reproductor o el servidor AUX que justifique
indexarlos para este trabajo. Se podrán volver a revisar si aparece una
necesidad específica.

## Revisión de actualizaciones de audio/protocolos — 2026-10-01

- [ProsperoRadio upstream, comparación desde la base fijada](https://github.com/blackbearreloaded/ProsperoRadio/compare/33898dd35375c1ae8370da137cfb6941d91c7684...89b9706afc78)
  — `main` está cinco commits por delante de `33898dd`. Los cambios están en
  README, roadmap, avisos de terceros y `tooling/native`; no tocan el reproductor,
  los codecs ni los protocolos. No hay un arreglo reciente que trasladar a DASH,
  HLS/AAC o MP3.
- [PS5 Audio Decoding Research, revisión `26be5a12`](https://github.com/blackbearreloaded/ps5-audio-decoding-research/commit/26be5a12f92b)
  y [PS5 hardware audio decoding, misma revisión](https://github.com/blackbearreloaded/ps5-hardware-audio-decoding/commit/26be5a12f92b)
  — la actualización reciente cambia README y documentación para describir el
  conversor AT9 como herramienta Python; no modifica la ruta de decodificación
  usada por Prospero Radio.
- [PS5_RetroArch, revisión `59a35aec`](https://github.com/mihawk-99/PS5_RetroArch/commit/59a35aec4ee0)
  — el commit más reciente revisado añade pruebas de entrada del mando. Sus
  cambios de interfaz, emulación y gráficos no aportan una corrección para
  reproducción de radio.
- [PS5_Vulkan, revisión actual `3f3ee696`](https://github.com/mihawk-99/PS5_Vulkan/commit/3f3ee6960701)
  — hay trabajo reciente en RADV/CTS y documentación gráfica. Esta integración
  afecta a la ruta Vulkan y no a los protocolos de audio; no actualizar el pin
  `085aac6` solo por los fallos DASH/AAC.
- [AudioC0re, revisión actual](https://github.com/MexrlDev/AudioC0re/commits/main)
  — proyecto de biblioteca musical local para LuaC0re: su README indica que el
  audio se convierte a PCM en PC y se transfiere a la consola por TCP. No es una
  implementación de streaming de radio ni una fuente directa para corregir los
  demuxers de Prospero Radio.

Resultado de esta revisión puntual: los cambios recientes inspeccionados no
contienen una corrección aplicable a los fallos de protocolo o codec reportados.
Esto no valida el DASH actual ni sustituye el análisis del ruido blanco con una
captura nueva del runtime.

## Referencia futura de imagen/vídeo

- [Merserk/dlss5-visual-enhancer](https://github.com/Merserk/dlss5-visual-enhancer)
  — añadido a petición del usuario para consulta futura sobre tratamiento de
  imagen/vídeo en PC. Su README lo describe como una aplicación Windows 11 para
  GPU NVIDIA RTX; no es una dependencia ni una referencia para la app nativa
  de PS5 o sus codecs de radio. Revisar licencia, requisitos y revisión concreta
  antes de reutilizar código.

## Cómo mantener el índice

Al usar una fuente en una futura corrección, añadir el archivo o módulo de
Prospero Radio afectado, la etiqueta `base`, `referencia` o `pista por
verificar`, y un enlace a un tag o commit concreto cuando se adapte código.
Registrar por separado las pruebas de host, el paquete producido y la evidencia
de PS5; no atribuir a la app el resultado de otro proyecto.
