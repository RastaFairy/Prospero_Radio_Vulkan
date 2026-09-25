# BUILD-FIX26 — compilación completa desde `build.ps1`

## Fallos encontrados

1. El enlace reportaba `nir_intrinsic_infos` y `nir_intrinsic_index_names` duplicados dentro de `libpsbc_driver.ps5.a`. El parche anterior añadía fuentes generadas que el `wildcard` del Makefile ya había encontrado.
2. Al quitar esos duplicados, el conversor encontraba `aco::instr_info` sin resolver. La fuente que lo define, `aco_opcodes.cpp`, también es generada y no estaba incluida en `ACO_SRCS` cuando Make evaluaba el `wildcard`.
3. El conversor nativo exigía stubs del SDK para símbolos ELF weak sin proveedor, aunque esas referencias débiles deben conservar su semántica opcional.

## Correcciones

- `setup-ps5-vulkan.sh` incorpora `aco_opcodes.cpp` y las fuentes NIR/SPIR-V necesarias con asignaciones inmediatas deduplicadas mediante `sort`. También elimina los objetos generados afectados antes de volver a crear el archivo y comprueba los símbolos NIR y ACO requeridos.
- `apply-vulkan.py` adapta el conversor para conservar símbolos indefinidos weak en la tabla ELF cuando no existe proveedor, sin exigirles un import del SDK. Los símbolos fuertes sin proveedor siguen siendo errores.

## Verificación

- `build.ps1` — PASS, modo `packages`, WSL2 Ubuntu 24.04, código de salida 0.
- PSBC — 16 fuentes compiladas en la ejecución final, 0 warnings.
- `eboot.bin` generado y validado como contenedor firmado íntegro.
- `PPSA99001.ffpkg` creado y validado como imagen UFS2.
- `PPSA99001.ffpfsc` creado; comprobación posterior: 0 warnings y 0 errores.
