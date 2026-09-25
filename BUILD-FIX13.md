# BUILD FIX13 — staging del auditor de símbolos

## Error observado en la ejecución real
La compilación llegó hasta `packages` y compiló `src/ps5_vulkan_renderer.cpp`, pero se detuvo antes del enlace final porque el worktree materializado no contenía:

`tools/check-vulkan-link-duplicates.py`

El propio `tools/build.sh` sí intentaba ejecutarlo.

## Causa
FIX12 añadió la llamada al auditor dentro del `tools/build.sh` generado, pero `overlay/apply-vulkan.py` solo copiaba `build-mesa-util.sh` desde `overlay/tools/`. Por tanto, el script auditor existía en el ZIP como `overlay/tools/check-vulkan-link-duplicates.py`, pero nunca se materializaba en `tools/` del checkout de ProsperoRadio.

## FIX13
- Se creó `stage_overlay_tools()` en `overlay/apply-vulkan.py`.
- La función copia tanto `build-mesa-util.sh` como `check-vulkan-link-duplicates.py`.
- La función verifica explícitamente que ambos archivos existan antes de continuar.
- El orden queda garantizado: primero `patch_target_build_driver()`, que crea `vulkan_archives` y `extra_objects`; después `patch_link_duplicate_audit()`.

## Validación
- `python3 -m py_compile overlay/apply-vulkan.py` — OK.
- Prueba sintética de staging + parcheado — PASS.
- La prueba sintética verificó que `check-vulkan-link-duplicates.py` aparece en el worktree y en el `tools/build.sh` generado.
- El `tools/build.sh` generado pasó `bash -n`.
- Se eliminaron los `__pycache__` del paquete antes de regenerar el ZIP.

## Estado
FIX13 corrige un fallo real de empaquetado del FIX12. El proyecto todavía no se considera compilado para PS5 hasta que una ejecución real produzca `eboot.bin` y complete la validación de paquetes.
