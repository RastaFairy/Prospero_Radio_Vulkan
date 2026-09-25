# BUILD FIX14 — limpieza del renderer Vulkan + resolución de `vkDeviceWaitIdle`

## Error observado en la ejecución real de FIX13

La compilación llegó al final del flujo de enlace. La auditoría de símbolos fuertes pasó:

```text
44 warnings generated.
Vulkan link symbol audit: no strong duplicate symbols
```

Después, el conversor del ejecutable rechazó una importación Vulkan:

```text
error: no public SDK stub exports required symbol vkDeviceWaitIdle
make: *** [Makefile:136: packages] Error 2
```

## Causa

`src/ps5_vulkan_renderer.cpp` llamaba directamente a `vkDeviceWaitIdle`. En la integración actual del título, el enlace estático resuelve el resto de las entradas Vulkan utilizadas, pero el conversor PS5 no encuentra `vkDeviceWaitIdle` entre los exports de los stubs públicos que recibe para construir la tabla de imports.

El renderer de ProsperoRadio solo crea una cola de gráficos y, cuando las familias son distintas, una cola de presentación. Vulkan define `vkDeviceWaitIdle` como equivalente a esperar a todas las colas del dispositivo con `vkQueueWaitIdle`.

## Cambios FIX14

### 1. Eliminación de los 44 warnings

43 avisos eran:

```text
-Wmissing-field-initializers
```

causados por expresiones del tipo:

```cpp
VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
```

Se añadió `MakeVkStruct<T>()`, que hace:

```cpp
T value{};
value.sType = type;
```

y se migraron los 43 inicializadores Vulkan a ese patrón. Se mantiene `pNext == nullptr` y el resto de los miembros quedan explícitamente a cero.

El aviso restante:

```text
-Wunused-parameter 'format'
```

provenía de `UploadTexture`. El parámetro `format` no se utilizaba allí y se eliminó de la declaración, definición y llamada. El formato continúa aplicándose en `CreateTextureFromPixels()` al crear la imagen Vulkan.

### 2. Sustitución de `vkDeviceWaitIdle`

El destructor ahora espera explícitamente:

```cpp
vkQueueWaitIdle(graphics_queue_);
```

y, si es una cola distinta:

```cpp
vkQueueWaitIdle(present_queue_);
```

No se introduce ningún stub falso ni una función que devuelva éxito sin sincronizar.

### 3. Nueva auditoría post-link de símbolos Vulkan

Además de la auditoría previa de duplicados, `tools/build.sh` ejecuta después de `prospero-lld`:

```bash
prospero-nm -D --undefined-only build/llvm-pie.elf
```

y filtra símbolos `vk[A-Z]`. Si queda cualquier símbolo Vulkan indefinido, el build falla inmediatamente y enumera todos los símbolos faltantes antes de llegar al conversor FSELF.

Esto está diseñado para evitar fallos sucesivos de "un símbolo por ejecución".

## Validación local realizada

- `python3 -m py_compile overlay/apply-vulkan.py` — PASS.
- Prueba sintética del staging de `check-vulkan-link-duplicates.py` — PASS.
- Prueba sintética de `patch_link_duplicate_audit()` — PASS.
- `bash -n` del `tools/build.sh` sintético con auditoría pre-link y post-link — PASS.
- 43 inicializadores `Vk*{VK_STRUCTURE_TYPE_*}` restantes — 0.
- llamadas directas a `vkDeviceWaitIdle` en el renderer — 0.
- declaración/definición/llamada de `UploadTexture` coherentes — PASS.

## Estado

FIX14 corrige el error de importación detectado en FIX13 y elimina el ruido de los 44 warnings del renderer.

No se declara todavía éxito final del proyecto: falta una ejecución real de `build.ps1` que alcance la generación y validación de `eboot.bin` y de los formatos de paquete.
