# ProsperoRadio Vulkan FIX4

## Error reportado

```text
fatal: cannot change to '.../ps5-opengl-sdk-0.3.0/third_party/opengnm-psbc': No such file or directory
...
subprocess.CalledProcessError: Command ['git', '-C', '.../third_party/opengnm-psbc', 'write-tree'] returned non-zero exit status 128.
```

## Causa real (verificada contra `blackbearreloaded/ps5-opengl` y `mihawk-99/PS5_Vulkan`, no contra su documentación)

`tools/build-psbc-ps5.sh` de `PS5_Vulkan` solo *verifica* el árbol de fuentes pinneado, offline:

```bash
(cd "$sdk" && python3 tools/fetch-sources.py --verify-psbc)
```

`--verify-psbc` llama directo a `verify_psbc()` y nunca pasa por `fetch_repo()`, que es lo único que clona `third_party/opengnm-psbc`. En un checkout de `ps5-opengl` recién clonado o solo actualizado (`git fetch` + `git checkout --detach`, que es lo que hace `overlay/setup-ps5-vulkan.sh`), `third_party/` ni siquiera existe todavía — de ahí el `No such file or directory` exacto que reportaste.

`overlay/setup-ps5-vulkan.sh` nunca ejecutaba el fetch real (sin `--verify-psbc`) antes de invocar `build-psbc-ps5.sh`. Ese paso faltaba.

## Corrección

`overlay/setup-ps5-vulkan.sh`, justo antes de `PS5_OPENGL_SDK="$OPENGL_INPUT" bash tools/build-psbc-ps5.sh`:

```bash
echo "==> PS5 OpenGL: fetching pinned third_party sources"
(cd "$OPENGL_INPUT" && python3 tools/fetch-sources.py)
```

`fetch_repo()` es idempotente (no re-clona ni resetea lo que ya coincide con el pin), así que es seguro ejecutarlo en cada build, no solo la primera vez.

## Verificación

Reproducido en un checkout real de `blackbearreloaded/ps5-opengl` (tag `v0.3.0`, commit `6cb291a`):

- **Sin el fetch**: `python3 tools/fetch-sources.py --verify-psbc` → mismo `fatal: cannot change to '.../third_party/opengnm-psbc'` que tu log.
- **Con el fetch** (`python3 tools/fetch-sources.py`, sin flag) **y luego** `--verify-psbc` → `PSBC: exact pinned source tree verified`.

## Nota aparte, no relacionada con tu error

El mismo `fetch-sources.py` también descarga `https://archive.mesa3d.org/mesa-26.2.0.tar.xz`. En este sandbox de verificación esa descarga dio `HTTP 403` porque `archive.mesa3d.org` no está en la lista de dominios permitidos del contenedor — no es un problema del pin ni de tu red WSL, solo una restricción de este entorno de prueba. No debería reproducirse en tu build real.

## Sigue sin probarse

Igual que en FIX3: nadie ejecutó esto con el toolchain PS5 real (`prospero-clang`). Este fix resuelve el error exacto que pegaste hasta el punto donde ocurrió; no garantiza que no aparezca un paso siguiente sin cubrir.
