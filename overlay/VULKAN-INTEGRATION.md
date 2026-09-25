# Integración Vulkan de ProsperoRadio

Este fork integra un renderer Vulkan nativo para ProsperoRadio en PS5. Mantiene fijado el código original en el commit `33898dd35375c1ae8370da137cfb6941d91c7684` y aplica los cambios de `overlay/` durante la compilación.

## Compilar

En Windows 11 con WSL2 y Ubuntu 24.04, desde PowerShell:

```powershell
.\build.ps1 -Mode packages
```

El lanzador admite `check`, `app`, `packages` y `clean`. Los paquetes aparecen en `out/`; el código fijado y la caché de dependencias se guardan fuera del repositorio. El build limpia los objetos generados cuando detecta cambios en el overlay o en el lanzador, para no reutilizar binarios obsoletos.

## Renderizado y resolución de salida

- RmlUi conserva un lienzo lógico de 1920×1080. Vulkan consulta la señal activa de VideoOut, selecciona el modo correspondiente y escala la interfaz a esa extensión.
- El fondo de la radio usa una textura KTX2 RGBA8 de 3840×2160 con 12 niveles mip. Vulkan selecciona el nivel de detalle y la carga se transmite por bloques; la compilación no fuerza la salida a 1080p.
- La ruta de PS5 utiliza Vulkan 1.0, presentación display/swapchain y enlaza estáticamente el runtime Vulkan. SDL sigue atendiendo entrada y temporización; su renderer por software no dibuja la interfaz RmlUi.

## Memoria y protección ante fallos

- La carga del fondo usa un staging buffer de 2 MiB y transmite filas, sin crear una copia RGBA 4K completa en la memoria de CPU.
- Cada uno de los dos frames de UI reserva 2 MiB para vértices y 1 MiB para índices. El pool de descriptores admite hasta 256 conjuntos.
- Las lecturas completas de archivos tienen un límite de 16 MiB; los archivos KTX2 continúan por la ruta de lectura en bloques. Si un recurso excede el límite, el log registra su ruta y tamaño, y la carga falla de forma controlada.
- Las cantidades de extensiones, dispositivos, modos de pantalla, colas e imágenes devueltas por Vulkan se validan antes de reservar memoria. La extensión de la salida también se valida sin reducir resoluciones válidas como 1440p o 4K.

## Controles y navegación

- Stick izquierdo arriba/abajo: subir/bajar volumen de AudioOut en pasos del 2 %, con repetición controlada.
- Stick derecho izquierda/derecha: sintonizar la emisora anterior/siguiente. Si ya hay reproducción, el servicio cambia a la nueva emisora al completar el cierre del stream activo.
- Cruceta izquierda/derecha: cambiar entre Popular, Tendencias, Mejor valoradas, Favoritos y Descubrir. L1/R1 ya no duplican esa función.
- Triángulo: abrir ajustes de radio; ahí se ven la lista actual, el total de emisoras, el volumen, los favoritos y la acción de actualizar el catálogo.
- Cruceta arriba/abajo y Cross recorren/activan las acciones de ajustes. Triángulo o círculo cierra la pantalla.
- Cross en una emisora inicia o pausa; cuadrado guarda/quita el favorito; Descubrir conserva búsqueda y filtros.

La versión 01.000.015 conserva las protecciones de lectura y recuentos del driver añadidas en 01.000.014. También corrige el flujo de controles y la pantalla de ajustes. `build.ps1 -Mode packages` compiló el eboot y generó FFPKG/FFPFSC; sus hashes y validaciones están registrados en el README. La ejecución en consola queda pendiente de instalar y lanzar esa versión.

La integración Vulkan se basa en el proyecto público `mihawk-99/PS5_Vulkan` y herramientas de compilación abiertas. `setup-ps5-vulkan.sh` y los avisos de upstream describen las fuentes y requisitos utilizados.
