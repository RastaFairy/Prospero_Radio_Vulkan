# Pendientes para la siguiente versión

Estado de referencia: árbol fuente fechado 2026-09-27. `overlay/apply-vulkan.py`
y `sce_sys/param.json` indican **01.000.027**. Esta lista separa resultados
comunicados por el autor de comprobaciones pendientes; no equivale a una certificación
de release.

## Ya observado como funcional en consola

El autor confirmó en la última prueba:

- El EQ permite edición y conserva sus valores.
- La reproducción vuelve a producir audio de forma fiable.
- La lista puede recorrer el catálogo completo, no solo la primera página.
- El autor confirma que □ guarda favoritos entre las primeras siete emisoras, pero
  falla al guardar emisoras posteriores. El arreglo aún no está implementado ni
  validado.

La versión exacta instalada no se contrastó con el árbol fuente durante esa prueba.
Hay que repetir estas comprobaciones al preparar una release.

## Bloqueantes

### P0 — Cerrar la compilación y el gate del paquete

- El intento registrado en `out/build028.log` creó el FFPFSC, pero acabó con error en
  `overlay/tools/verify-package.py`: el gate no encontró
  `out/PPSA99001/sce_sys/param.json`. Rastrear qué directorio produjo `make ffpfsc`,
  verificar la carpeta desplegable recién generada y corregir el contrato entre el
  build y el gate.
- Volver a ejecutar el flujo completo en limpio y exigir salida correcta del build y
  del gate. No etiquetar como release una compilación que termine en error.
- Comparar la versión de `apply-vulkan.py`, `param.json`, RML, banner del ELF y los
  recursos que entran realmente en el FFPFSC. Registrar SHA-256 y resultados.
- Alinear GitHub Actions con el flujo overlay documentado o añadirle un job específico:
  el workflow actual ejecuta objetivos `make` directamente y no demuestra que el
  build overlay de `build.sh` ni su gate queden cubiertos en CI.

### P1 — Reconstruir Ajustes y el cambio de tema

- En las pruebas recientes, Ajustes llegó a mostrarse como una tarjeta pequeña sobre
  una pantalla blanca; en otras, algunas filas y rótulos se solapaban. Integrar los
  ajustes en el LCD de la radio, respetar sus límites y mantener el foco claramente
  visible.
- El selector muestra nombres de acabados, pero el cambio no aplica de forma fiable
  las texturas. Confirmar que Walnut, Silver y Graphite estén en el paquete, carguen
  correctamente y cambien el frontal visible; si un tema no está disponible, no
  presentarlo como seleccionable.
- Comprobar navegación, confirmación y vuelta desde cada fila del menú en consola.

### P1 — Corregir la entrada del panel táctil

- El mapeo actual de toques del DualSense no selecciona los presets esperados.
- Contrastar offsets y estado de contacto con la estructura real del pad, detectar
  transiciones de pulsación/liberación y separar cada zona/preset.
- Verificarlo en hardware con un registro acotado de muestras; quitar el diagnóstico
  verboso antes de una build normal.

### P1 — Hacer funcionar la importación AUX desde la red

- El servidor de carga externa de listas no queda disponible en la consola.
- Registrar los resultados reales de inicialización de red, `socket`, `bind`,
  `listen` y aceptación de clientes; mostrar “listo” solo después de que el puerto
  esté escuchando.
- Probar una subida M3U desde otro dispositivo, respetar `Content-Length` y límites,
  importar las entradas al catálogo y mostrar errores de URL/formato. Confirmar una
  emisora conectándose a su URL real antes de indicar reproducción.

### P1 — Guardar favoritos en todas las páginas del catálogo

- En consola, □ permite guardar las primeras siete emisoras, pero no las que vienen
  después.
- Causa probable, inferida del código actual: `RadioApp::BuildList` crea índices de
  catálogo globales (`list_start_ + row`) y `HandleInput` los pasa a
  `radio_service_toggle_favorite`; sin embargo, `radio_service_query_page` guarda
  solo la página en `g_stations` y `radio_service_toggle_favorite` rechaza cualquier
  índice mayor o igual que `g_station_count` (cantidad de filas cargadas en esa
  página). Confirmar esta discrepancia y corregir la resolución del UUID global sin
  cambiar la identidad/orden de las emisoras.
- Criterio de aceptación: guardar y quitar favoritos antes y después del límite de
  página, cambiar de página/lista y volver, reiniciar la app y confirmar que la
  selección persiste y corresponde a la emisora correcta. Validar en consola.

## Pendientes de validación

### P2 — Indicador de auriculares

La animación del conector no refleja de forma fiable la conexión. Identificar una API
de sistema verificable para detectar la ruta/conexión de auriculares. Si el firmware
no ofrece una señal accesible, cambiar el indicador para que represente un estado que
la aplicación sí conozca, sin fingir detección física.

### P2 — Cierre de la aplicación

Un klog anterior registró `signal 12` durante el cierre. Volver a probar apagado y
salida cooperativa desde el estado actual, varias veces y con reproducción activa e
inactiva. Hasta entonces, tratar el cierre limpio como pendiente de regresión.

### P2 — Validar emisoras de extremo a extremo

La navegación por el catálogo completo está confirmada, pero el total del catálogo no
demuestra que cada stream esté disponible. Al sintonizar, mostrar por separado
selección, conexión, reproducción y error; probar URLs reales, cambios rápidos,
timeouts y recuperación sin dejar el estado atascado en “Tuning”.

## Criterio para cerrar una tarea

Cada corrección debe tener evidencia en su nivel: diff/código fuente, recursos y
metadatos generados, carpeta de app y FFPFSC, salida completa de compilación/gate, y
captura o log de la consola para los comportamientos dependientes de hardware. Una
compilación correcta no sustituye la prueba en PS5. Mantener las evidencias en los
documentos de validación sin convertir una hipótesis en resultado confirmado.
