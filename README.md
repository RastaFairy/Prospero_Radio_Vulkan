<div align="center">

<img src="docs/radio-front-hybrid-preview.png" alt="Vista previa de Prospero Radio" width="90%">

# Prospero Radio Vulkan

Radio por internet para PS5 con una interfaz inspirada en un receptor clásico.

![versión fuente](https://img.shields.io/badge/fuente-01.000.027-orange)
![plataforma](https://img.shields.io/badge/plataforma-PS5-00adef)
![estado](https://img.shields.io/badge/estado-en%20desarrollo-orange)
![licencia](https://img.shields.io/badge/licencia-GPL--3.0-orange)

[Cambios](CHANGELOG.md) · [Pendientes](docs/PENDIENTES.md) · [Memoria histórica](MEMORIA.md) · [Problemas](https://github.com/RastaFairy/Prospero_Radio_Vulkan/issues)

</div>

---

## Estado actual

La fuente del árbol indica **01.000.027** en `overlay/apply-vulkan.py` y
`sce_sys/param.json`. No hay una release 027/028 validada para distribución: el
intento 028 registrado en `out/build028.log` terminó con un error del gate del
paquete. Consulta [Pendientes](docs/PENDIENTES.md) antes de instalar o publicar
otra build.

En la prueba manual más reciente comunicada por el autor se confirmaron tres
correcciones en consola: edición y persistencia del EQ, reproducción de audio más
fiable y navegación por el catálogo completo. La correspondencia exacta entre esa
instalación y el estado fuente de este repositorio aún debe comprobarse. Los fallos
restantes están separados de esas observaciones en el backlog.

## Qué aporta este fork

- Renderer Vulkan integrado con el frontend RmlUi; SDL sigue atendiendo entrada y
  temporización.
- Interfaz de radio con catálogo, búsqueda, favoritos, metadatos del stream y controles
  visuales basados en texturas.
- Integración del driver Vulkan y sus herramientas mediante `overlay/`, aplicada a un
  commit upstream fijado en el build.
- Herramientas para producir la fachada y los atlas en
  [`docs/RADIO-CONTROLS-ATLAS.md`](docs/RADIO-CONTROLS-ATLAS.md).

La salida a distintas resoluciones se calcula a partir del modo activo de VideoOut;
su comportamiento en 4K, 1440p y modos dinámicos debe seguir validándose en consola.

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

Esta rama está en desarrollo y no ofrece una imagen 01.000.027/028 verificada para
instalar. Las futuras releases deben incluir la versión exacta validada, la carpeta
de aplicación o FFPFSC y sus SHA-256; el gate y la prueba de consola se describen en
[`docs/PENDIENTES.md`](docs/PENDIENTES.md).

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
y conserva su licencia GPL-3.0-or-later. La integración gráfica usa
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), Mesa, RmlUi y SDL. Consulta
`LICENSE` y los avisos de terceros incluidos en el repositorio para las condiciones
de redistribución de cada componente.
