# Pendientes de Prospero Radio

## Línea base y evidencia

- Las capturas compartidas por el usuario muestran `01.000.039` en pantalla. Esa es la build de los síntomas: lista M3U con textos superpuestos, ajustes sobre fondo claro, pestaña AUX poco visible y estado de preset memorizado que no se distingue bien. No hay en el `out/` actual un paquete v039 con hash que vincule esas capturas a un artefacto local.
- `01.000.041` se trató como candidata, pero no hay paquete v041 disponible en el `out/` actual ni una prueba atribuible a ella.
- `01.000.042` se compiló y el gate del paquete recién ensamblado pasó: 14 aprobados, 0 warnings y 0 fallos. Las fotos del usuario del 28-09-2026 muestran el sello v042 en PS5; no se cotejó el hash del paquete instalado, así que es evidencia parcial, no una certificación completa.
- El usuario confirmó previamente que EQ, reproducción, lista de estaciones más allá de la primera página, favoritos del catálogo, lógica de mando y color RGB del DualSense funcionan en las revisiones que probó. No inferir que toda esa evidencia corresponde a v042.
- No reintroducir ajustes ni selector de temas: el usuario los descartó. El indicador del jack no debe fingir una detección física.

## Pendientes por prioridad

### P1 — Evitar duplicados al volver a importar una M3U

**Complejidad media · riesgo: crecimiento repetido del catálogo AUX y resultados confusos.** El usuario reporta que reenviar una lista con emisoras ya importadas vuelve a agregarlas. Hacer la importación idempotente usando la URL de stream normalizada como identidad; no fusionar solo por nombre, porque una emisora puede publicar URLs distintas (bitrate o formato). Conservar todas las nuevas entradas únicas y comprobar que dos importaciones idénticas no cambien el número de estaciones.

### P1 — Corregir las etiquetas de las fuentes en el LCD

**Complejidad media · riesgo: no identificar la lista activa o seleccionar otra por error.** Las capturas v042 muestran el rótulo central vacío en una vista y `MEM / FAVORITOS` solapado con `AUX M3U` en otra. Revisar ancho, posición, texto real de cada fuente, foco/selección y sincronía con el modelo; probar RADIO, FAVORITOS y AUX M3U con distintos largos y sin datos.

### P1 — Probar persistencia de favoritos y presets AUX

**Complejidad media-alta · riesgo: estado que se pierde al reiniciar o preset que apunta a otra URL.** El código v042 guarda snapshots externos y favoritos AUX. Verificar en consola guardar/quitar favorito, memorizar y recuperar P1/P2/P3 desde una lista M3U, comprobar título y URL y reiniciar la app.

### P2 — Mejorar contraste del encabezado web AUX

**Complejidad baja · riesgo: subtítulo superior ilegible en pantallas pequeñas o con brillo bajo.** En la captura del panel de carga, el texto de modelo situado a la derecha pierde contraste con el metal. Ajustar su color/contraste sin cambiar el estilo de panel trasero aprobado.

### P2 — Detección física del jack del DualSense

**Complejidad alta · API sin verificar.** La pantalla v042 aún muestra `JACK N/A` con el mando presente. La búsqueda dinámica del símbolo es tentativa; no afirmar detección física hasta obtener transiciones reproducibles al conectar y desconectar los auriculares en PS5.

### P2 — Cierre, payload y persistencia tras reinicio

**Complejidad alta · riesgo: listener o payload huérfano y datos no persistidos.** Repetir con klog y log del bridge: detener AUX/puerto 7000, cerrar el payload, cerrar con POWER y con el menú del sistema, y verificar `/data/radio` tras reabrir. El build no acredita permisos ni ciclo de vida en hardware.

### P2 — Revisar restos del menú de ajustes/temas

El usuario descartó reintroducir ajustes y temas. La v042 no los presenta en los botones principales, pero el árbol conserva elementos ocultos de settings y copia recursos de temas. En una limpieza de código futura, comprobar su alcance antes de retirarlos para no romper el build; no son una función aprobada para la interfaz actual.

## Diferencias registradas

- **01.000.039:** build mostrada en las capturas del usuario; lista/textos, ajustes y señalización de presets presentaron los síntomas anteriores.
- **01.000.041:** candidata documentada para selector de fichero M3U y cambio de fuente en RADIO; no hay paquete v041 ni resultado de consola en el `out/` actual.
- **01.000.042:** capturas en consola confirman el sello de versión, emisora y listas visibles. En esas capturas se observan las pestañas defectuosas y `JACK N/A`; el usuario reporta duplicados tras reimportar una M3U. El gate local pasó, pero la validación de hardware es parcial.

Registrar por separado código, paquete, hash, gate, klog y observación en PS5. La etiqueta en pantalla identifica la versión mostrada, no el hash instalado; el gate no demuestra audio, acceso LAN ni persistencia en `/data`.
