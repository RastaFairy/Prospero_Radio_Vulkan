# BUILD-FIX18

## Fallo observado

La ejecución real posterior a FIX17 no llegó a compilar PSBC. El bootstrap alcanzó el parche de política y terminó en:

`RuntimeError: PS5 PSBC make invocation: expected exactly one match, found 0`

La causa estaba en `patch-ps5-vulkan-warning-policy.py`: la versión anterior intentaba encontrar la invocación completa de `make` como una única cadena multilínea. El `tools/build-psbc-ps5.sh` fijado en PS5_Vulkan contiene la misma orden funcional, pero el emparejamiento era demasiado literal y frágil.

## Corrección

FIX18 cambia el parche a sustituciones estructurales sobre fragmentos estables de los scripts fijados:

- PS5: inserta `-f "$warning_policy"` inmediatamente después de `-f "$root/tooling/psbc/support.mk"`.
- Host-PIC: inserta `-f "$warning_policy"` entre el Makefile de PSBC y `-j"$(nproc)"`.
- La política sigue usando un `.mak` externo, por lo que `CFLAGS += -w` y `CXXFLAGS += -w` se evalúan después de los Makefiles anteriores.
- `PS5VK_PSBC_WARNING_AUDIT=1` mantiene el modo de auditoría sin supresión.
- El cambio de versión `mesa-warning-policy-v3` invalida los objetos `.o`/`.a` del worktree para que el siguiente build no reutilice resultados construidos con la política anterior.

No se modifica el código fuente de Mesa/opengnm con esta corrección.

## Validación local

- `python3 -m py_compile overlay/tools/patch-ps5-vulkan-warning-policy.py` — OK.
- Prueba sintética contra la forma real fijada de `tools/build-psbc-ps5.sh` — parche aplicado — OK.
- Prueba sintética contra la forma real fijada de `tools/build-driver.sh` — parche aplicado — OK.
- `bash -n` de ambos scripts parcheados — OK.
- Aplicación doble del helper — idempotente.
- Se comprobó el contenido del parche: ambos `make` reciben `-f "$warning_policy"` exactamente una vez.

## Estado

FIX18 corrige el fallo del parche que impedía llegar a la compilación PSBC. Todavía no existe una ejecución real de FIX18 en la máquina Windows/WSL del usuario, por lo que no se debe afirmar todavía el número final de warnings PSBC ni el estado del enlace/paquetizado.
