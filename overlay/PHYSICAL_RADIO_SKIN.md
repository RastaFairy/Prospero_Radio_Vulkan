# Texturas físicas de Prospero Radio

El frontal se genera en vista ortográfica con diales circulares y centrados. La
pantalla usa cristal ahumado marrón ámbar mate, inspirado en radios de los 90;
el texto vivo se dibuja encima en tonos naranja LED. Ningún dato de emisora,
estado de reproducción ni carátula se graba dentro de la textura.

## Recursos

- `assets/ui/art/radio_front_master.png`: frontal plano de referencia a
  1920 × 1080.
- `assets/ui/art/radio_front_4k.tga`: fuente editable plana a 3840 × 2160.
- `assets/ui/art/radio_front_4k.ktx2`: textura plana runtime RGBA8 con 12 mips.
- `assets/ui/art/radio_front_1080p.tga`: referencia de 1920 × 1080; no usar
  como fondo runtime Vulkan.
- `assets/ui/art/radio_front_hybrid_4k.tga`: fuente editable del montaje híbrido.
- `assets/ui/art/radio_front_hybrid_4k.ktx2`: candidato runtime híbrido RGBA8,
  con 12 mips. Aún no es el recurso activo de `main.rml`.
- `assets/ui/controls/`: atlas TGA, `manifest.json` plano y
  `manifest-hybrid.json` con la geometría transformada.

## Generación

Desde la raíz del proyecto:

```powershell
python tools/build_radio_front_flat.py
python tools/build_radio_controls.py
python tools/build_radio_front_hybrid.py
```

Los generadores necesitan Pillow solo durante la autoría. El homebrew no usa
Pillow. El primer script actualiza el frontal y su KTX2; el segundo produce
atlas, manifiestos y hojas de revisión; el tercero monta la fachada sobre la
foto cálida y crea el KTX2 híbrido.

## Coordenadas e integración

El lienzo lógico es 1920 × 1080 con origen arriba a la izquierda. Para el fondo
plano, usa `manifest.json`; para `radio_front_hybrid_4k.ktx2`, usa únicamente
`manifest-hybrid.json`. No mezcles sus rectángulos. El display completo es
`[507, 353, 906, 292]` y el área segura para texto y carátula es
`[540, 392, 840, 216]` en el manifiesto plano.

La textura base ya contiene el cristal marrón oscuro. Mantén transparente el
fondo de las capas de texto y controles; recomienda `#F4BE76` para texto
principal, `#E2A658` para secundario y `#BC7E39` para metadatos tenues. Sin una
carátula real, usa todo el ancho de `screen.contentRect`.

El volumen tiene 21 pasos de 0 a 100 en incrementos de cinco. El control
derecho es anterior/siguiente digital y sus atlas solo tiñen las flechas de
ámbar; no se gira el dial ni se dibuja un marco alrededor. Las posiciones de
sprites, estados y padding están descritos en
`assets/ui/controls/manifest*.json`.

`build.sh` y `build-vulkan.sh` materializan el overlay con
`apply-vulkan.py`, que copia los dos KTX2 y los recursos TGA/JSON de controles.
Para el proceso y la capa RmlUi/Vulkan completos, consulta
`../docs/GLM-TEXTURAS-VULKAN.md` y `../docs/RADIO-CONTROLS-ATLAS.md`.
