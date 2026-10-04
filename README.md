<div align="center">

<img src="docs/prospero-radio-v042-portada.jpg" alt="Prospero Radio Vulkan 01.000.057" width="90%">

# Prospero Radio Vulkan

Radio por internet para PS5 con una interfaz inspirada en un receptor clásico.

![versión overlay](https://img.shields.io/badge/overlay-02.000.055-green)
![plataforma](https://img.shields.io/badge/plataforma-PS5-00adef)
[![Estado: Correcto](https://img.shields.io/badge/Estado-Correcto-success)](https://shields.io)
![licencia](https://img.shields.io/badge/licencia-GPL--3.0--or--later-green)

[Manual](docs/MANUAL-USUARIO.md) · [Flujos por módulos](docs/FLUJOS-OPERACION.md) · [Cambios](CHANGELOG.md) · [Pendientes](docs/PENDIENTES.md) · [Créditos](docs/CREDITOS.md) · [Fuentes técnicas](docs/FUENTES-TECNICAS.md) · [Contribuir](CONTRIBUTING.md) · [Licencia (ES)](LICENSE-ES.md) · [Integración Vulkan](VULKAN-INTEGRATION.md) · [Problemas](https://github.com/RastaFairy/Prospero_Radio_Vulkan/issues)

</div>

---

## Estado actual

La versión fuente actual y la build publicada más reciente son **01.000.046**.
El paquete FFPFSC se compiló el 01-10-2026, pasó el gate de recursos (14
aprobados, 0 avisos, 0 fallos) y su SHA-256 coincide con el asset de la release
[01.000.046](https://github.com/RastaFairy/Prospero_Radio_Vulkan/releases/tag/01.000.046):
`26B77747F9142472B0213B33E14FFAD435CB3A6B3B06157CAAD0848ADE4B8940`.

El usuario confirmó la v046 en PS5: la interfaz se mantiene ágil, reconoce la
configuración previa en `/data/radio` y la cruceta recorre la lista al mantener
arriba o abajo. Conserva también defectos de reproducción: el test DASH 06 se oye
como ruido blanco y algunos streams HLS fallan. La observación de consola no
equivale a validar todos los formatos o emisoras. Estado y límites en
[`docs/PENDIENTES.md`](docs/PENDIENTES.md).

## Qué aporta este fork

- Renderer Vulkan integrado con el frontend RmlUi; SDL sigue atendiendo entrada y
  temporización.
- Interfaz de radio con catálogo, búsqueda, favoritos, metadatos del stream y controles
  visuales basados en texturas.
- Integración del driver Vulkan y sus herramientas mediante `overlay/`, aplicada a un
  commit upstream fijado en el build.
- Herramientas para producir la fachada y los atlas en
  [`docs/RADIO-CONTROLS-ATLAS.md`](docs/RADIO-CONTROLS-ATLAS.md).
- Puente de persistencia en `/data/radio` y servidor de importación M3U por
  payload; el protocolo y sus límites están en
  [`docs/PUENTE-PAYLOAD.md`](docs/PUENTE-PAYLOAD.md).

La salida se calcula a partir del modo activo de VideoOut. La presentación final en
4K, 1440p y resoluciones dinámicas requiere validación en consola.

## Compilar

En Windows 11, instala WSL2 con Ubuntu 24.04 y ejecuta desde PowerShell:

```powershell
.\build-vulkan.ps1 -Mode ffpfsc
```

También puedes ejecutar `bash ./build.sh ffpfsc` desde Ubuntu/WSL. `ffpfsc` produce
la imagen comprimida; `app` compila y copia el directorio de la aplicación; `check`
ejecuta el objetivo de comprobación del proyecto y `clean` limpia el build upstream
aislado. `packages` se conserva como alias heredado de `ffpfsc`.

El build aplica `overlay/apply-vulkan.py` sobre el commit upstream fijado y conserva
la caché fuera del repositorio. Los resultados locales van a `out/`; no se deben
subir paquetes, dumps de consola, SDK ni dependencias descargadas. Consulta
[`VULKAN-INTEGRATION.md`](VULKAN-INTEGRATION.md) para conocer el flujo y sus límites.

## Distribución

Esta rama está en desarrollo y la v042 tiene validación parcial en hardware. El
paquete local pesa 52,887,552 bytes (SHA-256
`7C515C6EC3214429264E678B0E30391C370DA4F3E41048B863176A351051D3D7`). El sello de
pantalla confirma la versión mostrada, pero no identifica por sí solo el hash
instalado. No se distribuye aquí el paquete: los artefactos y logs de `out/` son
locales. Consulta [`CHANGELOG.md`](CHANGELOG.md) y
[`docs/PENDIENTES.md`](docs/PENDIENTES.md) para la evidencia y lo que falta.

## Estructura principal

| Ruta | Uso |
| --- | --- |
| `overlay/` | Parches, renderer, UI y recursos que consume el build Vulkan aislado. |
| `src/`, `include/`, `tooling/`, `vendor/` | Código y dependencias conservados del árbol base del proyecto. |
| `assets/ui/` | Recursos fuente del árbol del proyecto; los recursos del build overlay viven en `overlay/assets/ui/`. |
| `docs/` | Diseño de la interfaz, integración, pruebas y lista de trabajo pendiente. |
| `out/` | Builds, logs y evidencias locales; ignorados por Git salvo herramientas expresamente mantenidas. |

## Licencia y créditos

Este fork deriva de [ProsperoRadio de blackbearreloaded](https://github.com/blackbearreloaded/ProsperoRadio)
y conserva su licencia GPL-3.0-or-later. El texto íntegro está en inglés en
[`LICENSE`](LICENSE); [`LICENSE-ES.md`](LICENSE-ES.md) ofrece un resumen informativo
en castellano. La integración gráfica usa
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), Mesa, RmlUi y SDL. Consulta
[`docs/CREDITOS.md`](docs/CREDITOS.md) para los créditos, y `LICENSE` junto a los
avisos de terceros del repositorio para las condiciones de redistribución de cada
componente.
