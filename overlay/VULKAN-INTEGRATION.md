# Integración gráfica Vulkan

## Base del proyecto

La compilación utiliza la revisión `33898dd35375c1ae8370da137cfb6941d91c7684` de ProsperoRadio y aplica los cambios incluidos en `overlay/`. La versión de contenido generada es `01.000.013`.

## Ruta gráfica

RmlUi dibuja mediante un renderizador Vulkan 1.0 nativo. SDL Software Renderer se retira de la ruta de dibujo; SDL permanece para la entrada del mando y los temporizadores. El entorno de ejecución Vulkan para PS5 se enlaza estáticamente al título, sin cargarlo mediante `dlopen` en la consola.

El renderizador prepara una superficie y una cadena de intercambio de `VK_KHR_display`, consulta los modos que ofrece VideoOut y selecciona uno al iniciar. Escala el lienzo lógico de 1920×1080 al tamaño de presentación elegido. No fuerza una salida de 1080p.

## Textura y memoria

- Fondo KTX2 RGBA8 UNORM: 3840×2160, con 12 niveles mip.
- Muestreo Vulkan con niveles mip para adaptar el detalle al tamaño de presentación.
- Búfer temporal de transferencia de 2 MiB; las filas se transmiten sin crear una copia completa de la imagen 4K en CPU.
- Dos fotogramas en vuelo, cada uno con 2 MiB para vértices y 1 MiB para índices.
- Conjunto de descriptores limitado a 256 conjuntos.

La imagen maestra conserva su resolución. La salida depende del modo que el renderizador selecciona de los modos disponibles en VideoOut.

## Cómo compilar

En Windows 11 con WSL2 Ubuntu 24.04:

```powershell
.\build-vulkan.ps1 -Mode packages
```

Desde WSL o Linux:

```bash
bash ./build-vulkan.sh packages
```

Los modos disponibles son `packages`, `app`, `check` y `clean`. Los resultados se copian a `out/`; la copia base y las dependencias permanecen en la caché externa del usuario.

## Registro de la prueba en consola

En la prueba del 25 de septiembre de 2026, el paquete `01.000.013` superó la comprobación posterior de FFPFSC sin avisos ni errores. La captura de PS5 mostraba la interfaz orientada correctamente a 3840×2160, y el registro disponible no mostraba un cierre fatal durante una ventana de al menos cuatro minutos. Esto documenta esa prueba concreta; no garantiza estabilidad durante sesiones prolongadas.