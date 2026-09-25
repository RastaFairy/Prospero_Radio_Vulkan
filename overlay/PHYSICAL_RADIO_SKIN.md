# Interfaz con el frontal de la radio

## Textura y salida

- `assets/ui/art/radio_front_4k.ktx2` es la textura Vulkan RGBA8 UNORM cargada durante la ejecución. Tiene 3840×2160 píxeles y 12 niveles mip.
- `assets/ui/art/radio_front_4k.tga` es la imagen de referencia que usa el comprobador para verificar la textura KTX2.
- Al arrancar, el renderizador consulta los modos de vídeo que ofrece la consola mediante VideoOut y ajusta la presentación al modo seleccionado. El lienzo lógico de RmlUi es de 1920×1080.
- El sombreador de fragmentos usa muestreo con nivel de detalle implícito para seleccionar el nivel mip.

## Composición

- La imagen del frontal ocupa el fondo de la interfaz.
- El visor central muestra el nombre, los datos y el estado de reproducción de la emisora.
- El documento RML conserva los elementos que necesita la lógica de la aplicación. Los elementos auxiliares que no deben mostrarse quedan ocultos o fuera del área visible.
- El aspecto de los mandos y botones forma parte de la imagen del frontal. La navegación funcional sigue gestionada por los controles que ya implementa la aplicación.

## Presupuesto de memoria

La imagen KTX2 ocupa aproximadamente 42,2 MiB como archivo. La carga en CPU usa un búfer temporal de hasta 2 MiB. Cada uno de los dos fotogramas en vuelo reserva 2 MiB para vértices y 1 MiB para índices; el conjunto de descriptores se limita a 256 conjuntos.

El comprobador previo a la compilación valida las dimensiones, el formato Vulkan, el tamaño de los niveles mip y los límites del contenedor.