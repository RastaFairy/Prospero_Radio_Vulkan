# Atlas de controles de Prospero Radio

Paquete modular para el frontal ortográfico de Prospero Radio. Hay manifiestos
para el frontal plano y la variante híbrida sobre la fotografía. La fachada se
compone en vista frontal: los dos mandos son círculos concéntricos, están
centrados en el mismo eje y el display lleva cristal ahumado marrón ámbar mate,
con estilo de radio de los años 90. El texto dinámico usa el naranja de los
LED de los controles; no se hornean emisoras ni carátulas.

## Archivos

Los recursos de ejecución están en `overlay/assets/ui/controls/`:

- `volume.tga`: 21 overlays transparentes del indicador de volumen, de 0 a
  100 en pasos de 5, más una celda que contiene el aro de foco. La cara metálica
  y las marcas impresas permanecen en el fondo KTX2.
- `buttons.tga`: los siete botones físicos del frontal base, con
  estados normal, foco, pulsado, seleccionado y seleccionado con foco.
- `tuner.tga`: recolor ámbar de las flechas para foco y pulsación de anterior
  y siguiente emisora. No hay caja alrededor ni animación de giro.
- `album-art-frame.tga`: marco transparente para una carátula obtenida del
  stream o del catálogo, si está disponible.
- `manifest.json`: dimensiones, UV, estados, acciones y anclajes para el
  frontal plano anterior.
- `manifest-hybrid.json`: los mismos atlas con coordenadas transformadas al
  frontal aprobado sobre el fondo cálido. GLM debe usar este manifiesto junto
  con `radio_front_hybrid_4k.ktx2`.

La fuente es `overlay/assets/ui/art/radio_front_4k.tga`, creada por
`tools/build_radio_front_flat.py`. El atlas se genera con
`tools/build_radio_controls.py`. Ambos requieren Pillow solo para generar los
archivos; Pillow no es dependencia del homebrew.

Ambos manifiestos usan un lienzo lógico de 1920x1080. Para el frontal híbrido
se escala uniformemente la fachada 1.05835x y se centra; el cuerpo posterior
de la fotografía permanece visible. Los anclajes exactos están en cada JSON.
Los del frontal plano anterior son:

- Cristal completo: `[507, 353, 906, 292]`.
- Área segura para el contenido: `[540, 392, 840, 216]`.
- Centro del mando de volumen: `(365, 520)`; overlay de volumen: `[221, 376, 288, 288]`.
- Centro del mando derecho: `(1555, 520)`; anterior/siguiente digital: `[1472, 621, 166, 48]`.
- Teclas: siete rectángulos iguales en una fila centrada, definidos por tecla en `manifest.json`.

## Estados y selección

### Volumen

Hay un frame por cada valor `0, 5, 10, ... 100`. Se selecciona el más cercano al
volumen real sin alterar el valor que recibe AudioOut:

```cpp
const int frame = std::clamp(static_cast<int>(std::floor((volume + 2.5f) / 5.0f)), 0, 20);
```

Al ganar foco el control, se dibuja encima la celda `focusOverlay` del mismo
atlas. El indicador y el arco ámbar muestran el nivel, y el aro fino comunica
el foco activo.

### Tuner digital

La perilla dibujada permanece estática. Los símbolos impresos de anterior y
siguiente se resaltan como dos pulsadores digitales: el overlay cambia solo el
trazo de blanco cálido a ámbar, sin caja ni halo. Al mover el stick derecho a
izquierda/derecha, la acción salta una emisora y el frame pasa por foco o
pulsado; no se interpola un ángulo. El anclaje del atlas cubre ambos símbolos.

### Teclas

El atlas contiene una columna por tecla: Inicio, Radio, Favoritos, Géneros,
Buscar, Ajustes y Reproducir/Pausa. Cada fila es un estado. `selected` es la
sección persistente; `focus` es el control bajo navegación; `selected_focus`
combina ambos. `pressed` es la respuesta momentánea al botón.

## Integración en Vulkan/RmlUi

El cargador actual acepta TGA RGBA de 32 bits y KTX2. Los atlas TGA usan BGRA,
8 bits alfa, origen arriba-izquierda y un único mip. Cada frame tiene 8 px de
extrusión por lado fuera de su rectángulo UV; el UV apunta solo al núcleo del
frame. Ese borde evita que el muestreo lineal mezcle píxeles del vecino si el
filtro cambia. El sampler TGA Vulkan actual es nearest/clamp, así que el gutter
no cambia el aspecto ni crea márgenes visibles al dibujar el núcleo.

1. Para la propuesta híbrida, cargar `radio_front_hybrid_4k.ktx2` como primera
   capa de 3840x2160. Presentarla en el lienzo lógico completo de 1920x1080,
   con una única escala proporcional. Mantener `radio_front_4k.ktx2` como
   variante plana de recuperación.
2. Dibujar el nombre de emisora, canal/ubicación, idioma, códec, bitrate y
   canción actual dentro de `screen.contentRect`, en naranja ámbar sobre el
   cristal ahumado. Dejar transparente el fondo de esa capa y no cubrir la
   textura con otro panel. El manifiesto recomienda `#F4BE76` para texto
   principal, `#E2A658` para secundario y `#BC7E39` para metadatos tenues. El
   texto debe respetar ese rectángulo incluso al cambiar la resolución de
   VideoOut.
3. Si existe una carátula real proporcionada por la emisora o el stream,
   dibujarla dentro del cristal y reducir el ancho del texto. Si no existe,
   mantener oculto el elemento de carátula y usar todo el ancho para texto.
4. En RCSS declarar cada TGA con `@spritesheet`, tomando de JSON `x`, `y`,
   `width` y `height` del frame. No incluir los 8 px de extrusión en ese
   rectángulo. RmlUi resuelve los sprites por nombre y comparte la textura;
   no cargar una textura nueva por cada estado.
5. Elegir un frame de `volume.tga` en cada cambio de volumen y dibujarlo sobre
   el rectángulo indicado. Es un overlay con fondo transparente: no cubre el
   metal ni sustituye el dial fijo. Solo añade el arco, las marcas ámbar, la
   aguja y el aro de foco. Aplicar `focusOverlay` por encima mientras el
   volumen tenga el foco.
6. Dibujar el estado de `tuner.tga` sobre las flechas anterior/siguiente. El
   mando derecho nunca gira: es un pulsador digital de dos direcciones.
7. Seleccionar en `buttons.tga` una de las cinco filas y dibujarla sobre el
   rectángulo de cada tecla. La fila `selected_focus` combina ambos estados.

`overlay/apply-vulkan.py` copia ambos KTX2 candidatos y los `.tga`/JSON de
controles al árbol upstream que se empaqueta. La integración de estados todavía
corresponde al cambio de UI: el atlas no modifica por sí solo la navegación ni
el volumen. El proceso completo para Vulkan está en
`docs/GLM-TEXTURAS-VULKAN.md`.

## Regenerar y revisar

```powershell
python tools/build_radio_front_flat.py
python tools/build_radio_controls.py
python tools/build_radio_front_hybrid.py
```

El generador del frontal actualiza la textura plana y su KTX2; el de controles
regenera los atlas y manifiestos; el híbrido vuelve a componer la foto, el
frontal y la previsualización final. Las láminas de revisión son
`docs/radio-controls-atlas-preview.png` y `docs/radio-controls-composite-preview.png`.
La hoja con etiquetas es solo para revisión; los TGA no contienen títulos
añadidos ni datos de emisora estáticos.
