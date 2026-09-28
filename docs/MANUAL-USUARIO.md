# Manual de usuario — Prospero Radio 01.000.042

## Qué muestra este manual

Las pantallas son composiciones hechas con la fachada y los atlas de controles
del proyecto. No son capturas de una PS5. Los nombres y valores se contrastaron
con las fotos de la v042 que aportó el usuario; se usan como ejemplos de interfaz.

![Prospero Radio v042: fachada, reproducción y controles](prospero-radio-v042-portada.jpg)

## Controles del receptor

| Control | Acción |
| --- | --- |
| POWER | Inicia la secuencia de apagado y cierra la radio. |
| BANDA | Abre la lista de emisoras de Radio Browser. |
| MEM | Abre la lista de emisoras guardadas como favoritas. |
| AUX | Activa el servidor local para recibir una lista M3U. |
| BARRIDO | Lee la M3U recibida; pulsa **✕** para abrir sus emisoras. |
| EQ | Abre el ecualizador de cinco bandas. |
| PLAY/PAUSA | Inicia o detiene la reproducción. |
| Dial/joystick izquierdo | Ajusta el volumen; dentro de EQ modifica la ganancia de la banda seleccionada. |
| Dial/joystick derecho | Cambia de emisora; dentro de EQ selecciona la banda. |

En las listas, usa **↑/↓** para mover la selección, **✕** para sintonizar,
**□** para añadir o quitar una emisora de Favoritos y **○** para volver. Usa
**←/→** para alternar entre Radio, Favoritos y AUX M3U. Las etiquetas de esas
fuentes pueden aparecer vacías o solapadas en v042; la navegación sigue teniendo
esas tres fuentes aunque el rótulo no se vea bien.

![Ejemplo de lista y fuentes](manual/lista-fuentes-v042.jpg)

Una emisora que aparece en la lista M3U aún no está necesariamente conectada.
La app acepta URLs HTTP o HTTPS, pero comprueba el stream cuando se sintoniza.
Los nombres repetidos al volver a importar la misma lista siguen siendo un
pendiente conocido.

## Importar una lista M3U desde la red local

1. En la radio, abre **AUX** y deja activa la pantalla del servidor.
2. Desde un dispositivo conectado a la misma red, visita
   `http://IP-DE-LA-CONSOLA:7000/`.
3. Elige el fichero `.m3u` y envíalo. La página confirma la recepción.
4. Vuelve a la radio y pulsa **BARRIDO**. Cuando aparezca el número de emisoras,
   pulsa **✕** para abrir la fuente AUX M3U.
5. Selecciona una emisora y pulsa **✕** para intentar reproducirla.

La importación admite una carga de hasta 32 MiB. El servidor no tiene contraseña:
úsalo solo dentro de una red local de confianza. BARRIDO no certifica que todas
las URLs respondan; eso solo se sabe al intentar sintonizarlas.

![Pantalla BARRIDO tras recibir una M3U](manual/barrido-m3u-v042.jpg)

![Entrada AUX y puerto local](manual/aux-servidor-v042.jpg)

## Ecualizador

EQ presenta estas cinco bandas: **60 Hz, 250 Hz, 1 kHz, 4 kHz y 12 kHz**.
Mueve el dial derecho para seleccionar una y el izquierdo para cambiar su
ganancia. Pulsa **✕** para avanzar al siguiente preset; los cambios de ganancia
y preset se guardan desde la app.

![Ejemplo de valores del ecualizador observados en v042](manual/ecualizador-v042.jpg)

## Tres memorias rápidas del panel táctil

El área se divide en tres zonas: izquierda **P1**, centro **P2** y derecha
**P3**. El click físico del panel es obligatorio; tocar o deslizar sin pulsar no
cambia de emisora. La zona queda fijada al iniciar el click.

- **Click corto** en una zona: recupera su emisora memorizada. Si está vacía, la
  pantalla indica que no hay preset.
- **Mantén el panel pulsado 3 segundos** en una zona: guarda allí la emisora
  actual. Suelta antes y la grabación se cancela.
- La barra luminosa del DualSense comunica la memoria con pulsos ámbar; la
  cantidad identifica P1, P2 o P3.

La ilustración siguiente parte del render del DualSense elegido para esta guía;
las zonas y acciones se han rotulado para reflejar el funcionamiento de la radio.

![DualSense: zonas P1, P2 y P3 para recuperar y guardar presets](manual/dualsense-presets-v042-zonas-final.png)

## Límites conocidos de la v042

- Las pestañas de Radio, Favoritos y AUX M3U pueden tener el texto solapado o
  vacío.
- Al conectar auriculares al mando, la interfaz todavía puede indicar
  `JACK N/A`; la detección visual no está resuelta.
- Volver a importar una M3U puede añadir estaciones repetidas.
- La lista no verifica por adelantado cada stream; la disponibilidad se conoce
  al sintonizar.

El registro de tareas y su evidencia está en [`PENDIENTES.md`](PENDIENTES.md).
La guía de mandos y atlas está en [`RADIO-CONTROLS-ATLAS.md`](RADIO-CONTROLS-ATLAS.md)
y el flujo técnico del servidor en [`PUENTE-PAYLOAD.md`](PUENTE-PAYLOAD.md).
