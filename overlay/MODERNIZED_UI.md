# Interfaz modernizada de ProsperoRadio

Esta integración remodela la pantalla como una radio física sobre una televisión 16:9 y mantiene el renderer Vulkan de PS5.

## Composición

- Cuadrícula de emisoras de tres columnas por dos filas.
- Paneles planos y bordes sencillos para evitar efectos CSS que el renderer no admite.
- Fondo KTX2 RGBA8 de 3840×2160 con mipmaps, cargado por bloques.
- La salida conserva la resolución activa de VideoOut; la interfaz usa un lienzo lógico de 1920×1080 escalado al modo de salida.
- Las fuentes usan tamaños bitmap incluidos en el paquete y compatibles con BitmapFontEngine.

## Mandos

- Stick izquierdo arriba/abajo: ajustar el volumen real de AudioOut.
- Stick derecho izquierda/derecha: recorrer emisoras; si ya se está reproduciendo, cambia al nuevo stream.
- Cruceta izquierda/derecha: cambiar de lista.
- Cruceta arriba/abajo: recorrer emisoras y páginas.
- Triángulo: abrir ajustes con volumen, favoritos y actualización del catálogo.
- Cross: seleccionar o iniciar/pausar; cuadrado: alternar favorito; círculo: volver.
- La búsqueda y los filtros permanecen en la lista Descubrir.

La lista, el volumen y los estados de favoritos se obtienen del servicio real de la aplicación; la pantalla no contiene acciones de demostración.

## Compatibilidad gráfica

La UI evita sombras, degradados, transforms y radios de borde distintos de cero. Usa posiciones enteras, rellenos planos, los iconos TGA empaquetados y el motor de fuentes existente.
