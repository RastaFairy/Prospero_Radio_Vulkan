# Propuesta: frontal plano con profundidad fotográfica

## Dirección visual

La propuesta conserva el ambiente cálido de la escena original y coloca el frontal plano a escala de fachada completa. El cuerpo posterior puede asomar por arriba y por los lados: rejilla, carcasa y madera dan profundidad y mantienen la sensación de aparato físico. El frontal debe cubrir los mandos y la pantalla antiguos de borde a borde; no debe quedar reducido a una placa pequeña dentro del aparato.

La vista del frontal es ortográfica. Los dos diales conservan círculos reales, materiales metálicos planos y centros alineados. El dial izquierdo representa volumen; el control derecho es un selector digital anterior/siguiente. El display usa cristal ahumado marrón ámbar mate, inspirado en radios de los 90; los metadatos reales se dibujan encima con el naranja de los LED.

## Archivos de revisión

- `radio-front-hybrid-base.png`: escena cálida con el frontal plano completo, sin estados de navegación ni datos de ejemplo.
- `radio-front-hybrid-proposal.png`: vista 4K de ejemplo con volumen al 65 %, Favoritos seleccionado/en foco y el selector en siguiente/en foco.
- `radio-front-hybrid-preview.png`: la misma propuesta a 1920 × 1080.
- `radio-front-head-preview.png`: fuente fotográfica original.
- `overlay/assets/ui/art/radio_front_hybrid_4k.tga`: fuente 4K para edición y regeneración.
- `overlay/assets/ui/art/radio_front_hybrid_4k.ktx2`: candidato runtime RGBA8 para el backend Vulkan, con 12 niveles mip.
- `overlay/assets/ui/controls/manifest-hybrid.json`: coordenadas adaptadas al frontal ampliado; comparte los atlas TGA.
- `tools/build_radio_front_hybrid.py`: reconstruye las imágenes de revisión y los dos candidatos separados, sin sobrescribir el fondo activo.
- `tools/build_radio_controls.py`: produce los atlas TGA, ambos manifiestos y las hojas de revisión.

El KTX2 y el manifiesto híbridos quedan como candidatos separados. El RML activo sigue apuntando a `radio_front_4k.ktx2` hasta que GLM cambie y revise explícitamente la integración.

## Instrucciones para integrar el frontend

1. Usar la fachada plana a su ancho completo y escalarla de manera uniforme para la resolución activa. Centrarla en pantalla; no deformar el frontal ni aplicar perspectiva.
2. Mantener visible una parte del cuerpo posterior alrededor del frontal, como en la propuesta. Evitar bordes dobles o restos de la interfaz fotográfica antigua sobre los mandos nuevos.
3. Dibujar el nivel de volumen como un estado del dial izquierdo. El atlas incluye pasos de cinco puntos; el frontend debe seleccionar el paso correspondiente sin generar una imagen por cada valor.
4. Tratar el control derecho como selector digital: izquierda/anterior y derecha/siguiente cambian de emisora. Sus estados muestran foco, pulsación y dirección activa; el dial no rota.
5. Presentar el foco, la selección y la pulsación de Inicio, Radio, Favoritos, Géneros, Búsqueda, Ajustes y Reproducir/Pausa con los frames del atlas. La cruceta izquierda/derecha cambia de lista cuando la vista lo permita.
6. Dibujar en pantalla únicamente información disponible: nombre de emisora, ubicación, idiomas, tipo de audio, formato/bitrate, título e intérprete cuando el stream los proporcione y carátula solo cuando exista. No dejar los datos de ejemplo de esta propuesta grabados como contenido runtime.
7. Mantener separadas la imagen base, los overlays de estado, la pantalla dinámica y las regiones de interacción. Escalar los hitboxes con las mismas transformaciones que los elementos visibles.

La geometría, los atlas y los estados se documentan en `overlay/assets/ui/controls/manifest-hybrid.json` y `docs/RADIO-CONTROLS-ATLAS.md`. El proceso de empaquetado, carga RmlUi y muestreo Vulkan está detallado en `docs/GLM-TEXTURAS-VULKAN.md`.
