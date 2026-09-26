# Handoff para GLM: textura híbrida, atlas e integración Vulkan

## Objetivo y estado

La propuesta aprobada combina el fondo fotográfico cálido con la fachada plana completa. La rejilla y la carcasa trasera siguen visibles alrededor del frontal. La pantalla usa cristal ahumado marrón ámbar mate, inspirado en las radios de los años 90, con un resplandor naranja central muy tenue. El texto y los datos de emisora siguen siendo dinámicos y se dibujan encima en tonos LED ámbar.

Los recursos híbridos son candidatos separados. `main.rml` sigue apuntando al fondo actual `radio_front_4k.ktx2`; esta preparación no cambia todavía el fondo activo ni integra los estados de control. GLM puede cambiarlo de forma explícita y conservar el recurso plano como reversión.

## Archivos generados

Después de ejecutar los tres generadores en el orden indicado:

- `overlay/assets/ui/art/radio_front_hybrid_4k.tga`: fuente de trabajo RGBA de 3840 × 2160. Pesa más de 16 MiB; no cargar este TGA en el renderer.
- `overlay/assets/ui/art/radio_front_hybrid_4k.ktx2`: textura runtime 3840 × 2160, `VK_FORMAT_R8G8B8A8_UNORM`, imagen 2D sin compresión y 12 niveles mip completos. El cargador Vulkan la lee por bloques.
- `overlay/assets/ui/controls/buttons.tga`: 35 estados, 7 botones × 5 estados; frame de 320 × 128 px.
- `overlay/assets/ui/controls/volume.tga`: 21 pasos visuales de 0 a 100 en saltos de 5, más el aro de foco; frame de 400 × 400 px.
- `overlay/assets/ui/controls/tuner.tga`: reposo, foco y pulsación para anterior y siguiente; frame de 332 × 96 px (escala exacta 2× del rectángulo lógico de 166 × 48).
- `overlay/assets/ui/controls/album-art-frame.tga`: marco separado para carátula real opcional.
- `overlay/assets/ui/controls/manifest-hybrid.json`: UV y rectángulos lógicos que coinciden con la fachada híbrida ampliada.
- `overlay/assets/ui/controls/manifest.json`: conserva la geometría del frontal plano previo.

Los TGA de controles son de 32 bits, alfa de 8 bits, orden BGRA y origen arriba a la izquierda. El atlas de volumen pesa 16,613,394 bytes, por debajo del límite actual de 16 MiB del cargador de archivos completos.

## Por qué los atlas van en varias texturas

No combinar `buttons.tga`, `volume.tga`, `tuner.tga` y `album-art-frame.tga` en un único TGA. Juntos ocupan 24,576,328 bytes (23.44 MiB); el renderer limita las lecturas completas a 16 MiB, así que un atlas RGBA sin pérdida con todos esos píxeles sería rechazado por `LoadTexture`. Aumentar ese límite obligaría al cargador a decodificar de una vez una imagen mayor sin reducir la memoria GPU total, que depende de la misma cantidad de texels.

La organización ya tiene un solo mapa lógico: el manifiesto elegido (`manifest.json` o `manifest-hybrid.json`) contiene botones, volumen, tuner, marco de carátula y pantalla. Los tres TGA de estados son tres sprite sheets, cargados una vez cada uno; todos los frames de un sheet reutilizan esa textura mediante sus UV. El marco de carátula es un recurso opcional aparte. No se crea ni se carga una textura por botón, estado o frame.

## Espaciado y UV: no recortar el padding

Todos los frames de `buttons.tga`, `volume.tga` y `tuner.tga` tienen **8 px de extrusión por lado**. El generador copia los texels del borde hacia ese padding. Cada rectángulo de `manifest*.json` señala solo el núcleo del frame; por ejemplo, el primer botón empieza en `[8, 8]`, no en `[0, 0]`. La separación entre núcleos vecinos es de 16 px, formada por los 8 px de cada frame.

Al definir sprites, usar exactamente `x`, `y`, `width` y `height` del JSON. No sumar el padding al rectángulo, no dividir la hoja en celdas sin leer las coordenadas y no muestrear el gutter como parte de la imagen. Así el filtro no recoge los bordes de otro botón y el padding no aparece como un margen visible.

Los atlas TGA no llevan mipmaps. El sampler de una textura TGA en `Ps5VulkanRenderInterface` usa actualmente `VK_FILTER_NEAREST` y `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE`; los 8 px extruidos son un margen de seguridad si se usa filtrado lineal en un cambio posterior. No generar una pirámide mip global de un atlas: sus niveles bajos mezclarían celdas distintas.

## Proceso reproducible

Desde la raíz del proyecto, ejecutar en este orden:

```powershell
python tools/build_radio_front_flat.py
python tools/build_radio_controls.py
python tools/build_radio_front_hybrid.py
```

El primer paso genera el frontal ortográfico y actualiza el KTX2 plano, incluidos sus mipmaps. El segundo produce los cuatro TGA de controles, ambos manifiestos y las hojas de atlas/composición. El tercero monta la fachada sobre la fotografía a 4K, guarda el TGA de autoría, escribe el candidato KTX2 con la cabecera compatible y reconstruye todos sus niveles mip, y genera las vistas de revisión. Si solo cambian los estados de un atlas, se pueden regenerar los controles y la composición sin rehacer la geometría plana.

Después de cambiar un control, geometría o padding, volver a generar los atlas y tomar sus UV del manifiesto recién creado. No editar manualmente únicamente el JSON o una imagen generada: el siguiente proceso volvería a desalinearlos.

`build.sh` y `build-vulkan.sh` ejecutan primero `overlay/check-runtime-texture-budget.py` y luego `overlay/apply-vulkan.py`. El preflight toma el KTX2 seleccionado por `#radio-backdrop` y valida su TGA fuente emparejado, cabecera, niveles mip y presupuesto; acepta tanto `radio_front_4k.ktx2` como `radio_front_hybrid_4k.ktx2`. También se puede revisar la candidata híbrida sin cambiar `main.rml`:

```powershell
python overlay/check-runtime-texture-budget.py radio_front_hybrid_4k.ktx2
```

`overlay/apply-vulkan.py` copia ambos KTX2 y los TGA/JSON de `controls/` al árbol upstream materializado. No hace falta añadir el TGA 4K de autoría al paquete runtime.

## Integración en RmlUi/Vulkan

1. Mantener el lienzo lógico y los hitboxes en 1920 × 1080. Cambiar temporalmente la fuente del elemento `#radio-backdrop` en `overlay/assets/ui/main.rml` a `art/radio_front_hybrid_4k.ktx2`. No cambiar a TGA ni reducir la fuente a 1080p.
2. Usar `manifest-hybrid.json` para el display, los diales, las siete teclas y el selector digital. No combinar estos rectángulos con los de `manifest.json`: la fachada híbrida mide 1850 px lógicos de ancho y las posiciones cambiaron con el escalado uniforme.
3. Declarar cada atlas una sola vez en `app.rcss` mediante `@spritesheet`, usando las coordenadas núcleo del JSON. La copia incluida de RmlUi 6.2 expone `Spritesheet` y admite `decorator: image(nombre-del-sprite)`. Consulte la documentación oficial de [spritesheets](https://mikke89.github.io/RmlUiDoc/pages/rcss/sprite_sheets.html) y [decoradores de imagen](https://mikke89.github.io/RmlUiDoc/pages/rcss/decorators/image.html).
4. Dar a cada elemento el `targetRect` del manifiesto como posición y tamaño lógico. Mantener una única clase visual por tecla y resolver su fila con esta prioridad: pulsada, seleccionada y enfocada, seleccionada, enfocada, normal. Mientras se pulsa se muestra `pressed`; al soltar, se restaura la combinación de selección y foco que corresponda. `selected_focus` es un frame combinado, no se superpone con `selected` ni con `focus`. No crear un recurso Vulkan por frame.
5. Mantener cargado cada TGA atlas una vez durante la vida del documento. RmlUi resuelve cada sprite como una región de esa textura; el backend existente recibe su geometría/UV por `RenderGeometry`. Liberar los recursos al cerrar el contexto, no al cambiar de emisora ni de frame.
6. Conservar el orden de capas: KTX2 base; overlay de volumen; foco del volumen; estado de anterior/siguiente; botones; texto y carátula real dentro de `screen.contentRect`.
7. Para el stick izquierdo, elegir el frame de volumen más cercano a múltiplos de cinco. El atlas no sustituye el valor real que recibe AudioOut. Para el stick derecho, anterior/siguiente cambian emisora; la perilla derecha no gira. La cruceta izquierda/derecha sigue cambiando de lista.
8. El cristal ahumado ya forma parte de la textura base: dejar transparente el fondo de la capa de contenido y no dibujar encima otra placa. El manifiesto sugiere `#F4BE76` para el texto principal, `#E2A658` para el secundario y `#BC7E39` para metadatos tenues. Mantener el mismo naranja ámbar que los LED de los controles. Mostrar `album-art-frame.tga` y la carátula solo si llega una imagen válida; sin carátula, usar todo el ancho para texto. No dejar textos de las previsualizaciones en el RML.
9. Centrar y escalar toda la composición con la misma escala X/Y según la salida VideoOut. El quad de fondo conserva sus mipmaps; no cambiar el viewport a una resolución fija para compensar texturas.

Ejemplo de declaración RCSS para el primer botón y dos estados. Las coordenadas son del núcleo y el resto debe generarse desde el JSON:

```rcss
@spritesheet radio-buttons {
    src: ../controls/buttons.tga;
    home-normal: 8px 8px 320px 128px;
    home-focus: 8px 152px 320px 128px;
    home-pressed: 8px 296px 320px 128px;
    home-selected: 8px 440px 320px 128px;
    home-selected-focus: 8px 584px 320px 128px;
}

#home-key { decorator: image(home-normal); }
#home-key.focused { decorator: image(home-focus); }
#home-key.pressed { decorator: image(home-pressed); }
#home-key.selected { decorator: image(home-selected); }
#home-key.selected.focused { decorator: image(home-selected-focus); }
```

Las filas del JSON son `normal`, `focus`, `pressed`, `selected` y `selected_focus`; el nombre CSS `.focused` se mapea a la fila `focus`. Para cada frame, toma `x`, `y`, `width` y `height` directamente del manifiesto; el ejemplo solo ilustra la primera columna. El atlas de volumen guarda 0, 5, …, 100 y un overlay de foco separado. El atlas del tuner es un interruptor: `idle`, `previous_focus`, `previous_pressed`, `next_focus` y `next_pressed`. Sin dirección activa se muestra `idle`; al inclinar el stick, se muestra el estado `*_focus`; en el instante de ejecutar anterior/siguiente, `*_pressed`. Los dos estados activos recolorean únicamente el trazo impreso de la flecha de blanco cálido a ámbar; no dibujan recuadro, fondo ni halo alrededor.

## Carga Vulkan que debe mantenerse

- El camino KTX2 reconoce `VK_FORMAT_R8G8B8A8_UNORM`, recorre 12 mips y transmite los datos por bloques de staging. Mantener la carga existente; no expandir el KTX2 a un buffer completo ni reconstruirlo en el arranque.
- Los TGA RGBA se leen como BGRA8 y crean una imagen Vulkan de un mip. Sus dimensiones y sus bytes quedan bajo el límite completo del renderer. La textura 4K TGA de la escena solo es fuente de trabajo; el camino runtime es KTX2.
- Conservar el sampler mipmapped para el KTX2 y el sampler nearest/clamp actual para los atlas TGA. No aplicar mipmaps compartidos a celdas independientes.
- Mantener PNG de composición, manifiestos y fuentes TGA de autoría fuera de cualquier carga por frame. Ningún estado requiere volver a subir texturas mientras navega el usuario.

## Revisión de integración

Antes de darlo por integrado, comprobar en el build de escritorio o consola que: el fondo cálido conserva la orientación; el cuerpo posterior sigue asomando; la placa cubre todos los mandos viejos; los siete botones y sus focos quedan dentro de sus hitboxes; el dial izquierdo muestra cada paso de cinco sin tapar el metal; las dos flechas del dial derecho cambian emisora sin rotación; el texto no sale del cristal; y no aparecen líneas/márgenes de celdas vecinas al cambiar de estado. Si un overlay no coincide, corregir el layout desde `manifest-hybrid.json`, no deformar la textura ni editar UV a ojo.
