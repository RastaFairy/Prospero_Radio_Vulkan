# BUILD-FIX20

## Fallo observado en FIX19

El build no llegaba a compilar PSBC. El checkout cacheado de PS5_Vulkan conservaba modificaciones tracked de una ejecución anterior del overlay porque `setup-ps5-vulkan.sh` hacía `git checkout --detach` pero no `git reset --hard`.

Eso dejaba `tools/build-psbc-ps5.sh` y/o `tools/build-driver.sh` en una versión ya parcheada. El helper siguiente buscaba la forma original del `make` y encontraba 0 coincidencias.

## Corrección

### `overlay/setup-ps5-vulkan.sh`

Después de:

```bash
git fetch --depth 1 origin "$PS5_VULKAN_REF"
git checkout --detach "$PS5_VULKAN_REF"
```

se añade:

```bash
git reset --hard "$PS5_VULKAN_REF"
```

Esto restaura los ficheros tracked al commit fijado antes de aplicar nuevamente el overlay. Los artefactos de build ignorados por Git no se eliminan.

### `overlay/tools/patch-ps5-vulkan-warning-policy.py`

La política pasa a `mesa-warning-policy-v5` y se añade un guard defensivo que detecta cualquier `mesa-warning-policy-vN` anterior a la versión actual. El error resultante identifica el estado residual y exige que el checkout sea restaurado al commit.

La modificación de provenance sigue siendo opcional, y las inserciones funcionales de `-f "$warning_policy"` continúan siendo estrictas.

## Validación local

- `python3 -m py_compile` — OK.
- `bash -n` de `setup-ps5-vulkan.sh` — OK.
- Aplicación de v5 sobre scripts sintéticos con la forma real de los `make` upstream — OK.
- `bash -n` de ambos scripts sintéticos parcheados — OK.
- Segunda aplicación de v5 — idempotente.
- Guard de `mesa-warning-policy-v4` — produce error explícito y no el anterior `expected exactly one match, found 0`.

## Estado

FIX20 corrige el estado persistente del checkout cacheado y elimina la ambigüedad del diagnóstico del warning-policy helper. No se afirma todavía un build completo hasta ejecutar `build.ps1` con FIX20.
