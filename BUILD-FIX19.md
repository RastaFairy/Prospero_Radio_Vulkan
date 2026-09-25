# BUILD-FIX19

## Fallo observado

La ejecución real de FIX18 llegó al parche de la política y falló después de aplicar correctamente la modificación de la invocación `make`:

`RuntimeError: PS5 provenance warning line: not found`

La causa es el segundo paso opcional del helper `patch-ps5-vulkan-warning-policy.py`: intentaba modificar una línea concreta de `PROVENANCE.txt`. Esa línea puede haber sido reescrita por una etapa anterior del overlay antes de que se ejecute este helper. El cambio de provenance no es necesario para aplicar `-w`/`-f "$warning_policy"` ni para compilar PSBC.

## Corrección FIX19

- Versión de política: `mesa-warning-policy-v4`.
- La modificación de provenance pasa a ser opcional: si la línea canónica existe, se actualiza; si no existe, el helper continúa sin error.
- La aplicación de la política PS5 y host-PIC sigue siendo estricta: si no se encuentra el `tree`/`work` o la invocación estable de `make`, el helper sigue fallando.
- Se conserva `PS5VK_PSBC_WARNING_AUDIT=1` para auditar warnings del código de terceros sin la supresión `-w`.
- El cambio de versión fuerza otra reconstrucción de los objetos `.o`/`.a` del worktree de PSBC.

## Validación local

- `python3 -m py_compile overlay/tools/patch-ps5-vulkan-warning-policy.py` — OK.
- Prueba sintética con la línea de provenance ausente — OK.
- Segunda ejecución sobre el resultado ya parcheado — idempotente — OK.
- El parche sigue insertando `-f "$warning_policy"` en PS5 y host-PIC.

## Estado

FIX19 elimina el último fallo conocido del bootstrap de la política de warnings observado en la ejecución real. Todavía no se declara éxito del build PSBC, del enlace Vulkan ni del empaquetado hasta recibir una ejecución real posterior.
