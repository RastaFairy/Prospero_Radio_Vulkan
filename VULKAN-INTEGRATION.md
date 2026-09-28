# Integración Vulkan

Prospero Radio sustituye el renderer SDL por una ruta Vulkan para presentar la UI de
RmlUi en PS5. SDL se conserva para entrada y temporización. El build overlay trabaja
sobre el commit upstream fijado en `overlay/apply-vulkan.py`; `overlay/` es la fuente
de los cambios que aplica ese flujo.

## Compilación del overlay

Requisitos: Windows 11 con WSL2 y Ubuntu 24.04, o un entorno Ubuntu compatible con el
toolchain PS5 del proyecto.

Desde PowerShell:

```powershell
.\build-vulkan.ps1 -Mode ffpfsc
```

Desde Ubuntu/WSL:

```bash
bash ./build.sh ffpfsc
```

Modos admitidos: `ffpfsc` (imagen comprimida), `probe-ffpfsc` (variante de
diagnóstico con payload local), `app` (carpeta de aplicación), `check` y `clean`.
`packages` se acepta como alias antiguo de `ffpfsc`. El build ejecuta el
overlay, prepara el driver y copia los resultados locales a `out/`. Mantiene el
checkout upstream y la caché de dependencias fuera del árbol Git. Los resultados de
`out/` no se deben subir al repositorio.

`build-vulkan.sh` queda como lanzador de compatibilidad y delega en `build.sh`; ambos
deben seguir usando el mismo gate y el mismo flujo para evitar builds divergentes.

## Resolución y presentación

La UI usa un lienzo lógico de 1920×1080. El renderer consulta el modo de VideoOut y
calcula la extensión de presentación a partir de la señal activa. La textura maestra
de la fachada es 3840×2160 y la ruta Vulkan la carga por bloques. La salida 4K,
1440p, 1080p y los cambios dinámicos deben comprobarse en consola; la resolución de
la señal no prueba por sí sola que la composición se haya escalado correctamente.

Los controles y fondos usados por este build están bajo `overlay/assets/ui/`. La guía
del atlas documenta sus coordenadas y recursos. En una auditoría, distingue siempre
la fuente, los archivos generados, la carpeta de app, la imagen FFPFSC, el log de
runtime y la captura de PS5.

## Memoria y límites

- El fondo KTX2 utiliza carga por bloques; el tamaño de lectura completa y los
  presupuestos de textura se comprueban antes de cargar.
- El runtime incorpora un pool de Direct Memory para asignaciones grandes. Los
  límites y los fallos quedan registrados en el log de la app.
- El gate `overlay/tools/verify-package.py` debe ejecutarse contra una carpeta de
  aplicación recién generada. No sustituye la comprobación del FFPFSC, su versión ni
  la prueba en consola.

## Estado de validación

La fuente actual declara `01.000.042`. El flujo `./build.sh ffpfsc` terminó con
código 0; el empaquetador informó 0 errores y 0 warnings, y el gate del paquete
recién generado registró 14 aprobados, 0 warnings y 0 fallos. La carpeta y el
FFPFSC locales se encuentran bajo `out/prospero-radio-01.000.042/`.

Las capturas del usuario del 28-09-2026 muestran `01.000.042` en el LCD y la app
activa en consola. La validación es parcial: el texto del estado de auriculares
continúa como `JACK N/A`, hay defectos visibles en las pestañas de fuentes y se
reporta duplicación al volver a enviar una M3U. No hay una comparación del hash del
paquete instalado con el generado localmente. Consulta
[`docs/PENDIENTES.md`](docs/PENDIENTES.md) para los resultados por función.

La integración se basa en [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan),
Mesa, [RmlUi](https://github.com/mikke89/RmlUi) y SDL. Los scripts de preparación y
los avisos de terceros de `overlay/` registran las fuentes utilizadas.
