# BUILD-FIX17

## Motivo

La ejecución de FIX16 reveló dos problemas de implementación:

1. El parche `mesa-warning-policy-v1` se insertaba en el script PS5 antes de que `tree` estuviera definido, provocando:
   `tools/build-psbc-ps5.sh: line 36: tree: unbound variable`.
2. Los 119 warnings PS5 y 114 warnings host-PIC pertenecen al árbol de compilación de PSBC/Mesa (tercero/pinned), no al renderer del título. FIX16 no llegó a aplicar la política porque abortó antes de compilar.
3. El bloqueo independiente del título seguía siendo que `src/ps5_vulkan_renderer.cpp` llamaba directamente a 85 entry points Vulkan, mientras PS5_Vulkan proporciona el acceso de aplicación a través de `vkGetInstanceProcAddr`. Eso dejaba los entry points como referencias fuertes indefinidas en el ELF final.

## Cambios

### PSBC warning policy v2

- Se mueve la inserción de la política para que `tree` y `work` estén definidos antes de usarlo.
- Cambia la versión a `mesa-warning-policy-v2` para invalidar los objetos compilados con la política anterior.
- Cuando cambia la política, elimina objetos `.o` y archivos `.a` del worktree de PSBC para forzar una recompilación real.
- Por defecto, PSBC (código Mesa/opengnm versionado) se compila con `-w`, dejando el build limpio sin editar cientos de fuentes de terceros solo para diagnóstico.
- Se añade `PS5VK_PSBC_WARNING_AUDIT=1` para ejecutar una compilación de auditoría sin esa supresión.
- El log/provenance sigue contando warnings y categorías cuando el modo de auditoría está activo.
- La misma política se aplica al PS5 PSBC y al host-PIC `libpsbc.pic.a`.

Esto es una política de compilación del código de terceros; no altera los warnings del renderer ni del código propio de ProsperoRadio.

### Vulkan static dispatch

- Se añade `VulkanDispatch` al renderer con los 85 entry points que realmente usa.
- `vkCreateInstance` y `vkEnumerateInstanceExtensionProperties` se cargan con `vkGetInstanceProcAddr(VK_NULL_HANDLE, ...)` antes de crear la instancia.
- Después de crear la instancia, el renderer carga los 83 entry points restantes mediante `vkGetInstanceProcAddr(instance_, ...)`.
- Todas las llamadas directas del renderer se redirigen al dispatch.
- Si falta un entry point, el renderer informa exactamente cuál en lugar de dejar una referencia indefinida en el ELF.
- `vkDeviceWaitIdle` sigue eliminado; el cleanup usa `vkQueueWaitIdle` sobre las colas que realmente utiliza el renderer.

## Validación local

- `python3 -m py_compile` de los scripts de overlay: OK.
- `bash -n` de los scripts parcheados sintéticos: OK.
- Aplicación del warning policy v2 dos veces: idempotente.
- El policy force-rebuild usa extensiones reales `.o`/`.a`.
- Auditoría estática del renderer: 85 llamadas Vulkan → 85 miembros `VulkanDispatch`, 0 faltantes, 0 llamadas directas no despachadas.

## Estado

No se declara todavía éxito de compilación final. La siguiente ejecución real debe confirmar:

- PSBC recompilado sin warnings de salida por defecto.
- PS5 driver y host-PIC reconstruidos con la nueva política.
- Link del título sin el bloque de entry points Vulkan indefinidos.
- Conversión a `eboot.elf`, generación de `eboot.bin` y paquetes FFPKG/FFPFSC.
