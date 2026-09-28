# Pendientes de Prospero Radio

## Línea base y evidencia

- Las capturas compartidas por el usuario muestran `01.000.039` en pantalla. Esa es la build de los síntomas: lista M3U con textos superpuestos, ajustes sobre fondo claro, pestaña AUX poco visible y estado de preset memorizado que no se distingue bien. No hay en el `out/` actual un paquete v039 con hash que vincule esas capturas a un artefacto local.
- `01.000.041` se trató como candidata, pero no hay paquete v041 disponible en el `out/` actual ni una prueba atribuible a ella.
- `01.000.042` se compiló y el gate del paquete recién ensamblado pasó: 14 aprobados, 0 warnings y 0 fallos. Las fotos del usuario del 28-09-2026 muestran el sello v042 en PS5; no se cotejó el hash del paquete instalado, así que es evidencia parcial, no una certificación completa.
- El usuario confirmó previamente que EQ, reproducción, lista de estaciones más allá de la primera página, favoritos del catálogo, lógica de mando y color RGB del DualSense funcionan en las revisiones que probó. No inferir que toda esa evidencia corresponde a v042.
- El usuario informa una degradación grave de rendimiento tras varias horas de reproducción. El klog de v042 registra 1.006 errores `filesystem full` del proceso de la app entre 20:25:31.621 y 20:25:52.456. El klog no identifica el archivo o montaje afectado; la posible relación con el log de runtime sigue siendo una hipótesis. Ver [`DIAGNOSTICO-RENDIMIENTO-2026-09-28.md`](DIAGNOSTICO-RENDIMIENTO-2026-09-28.md).
- No reintroducir ajustes ni selector de temas: el usuario los descartó. El indicador del jack no debe fingir una detección física.

## Pendientes por prioridad

### P1 — Evitar agotamiento del sistema de archivos y lentitud tras varias horas

**Complejidad media · impacto alto.** El usuario observó que, tras varias horas de reproducción, la aplicación se vuelve extremadamente lenta. El klog de consola asociado a la revisión v042 contiene 1.006 mensajes `filesystem full` para `pid 122 (eboot.bin)`, desde 20:25:31.621 hasta 20:25:52.456. Como evidencia separada, el `prospero-radio.log` local de v042 ocupa 11.598.333 bytes y contiene 29.019 avisos repetidos de fuente `Montserrat [bold]` no definida; su hora/ejecución no coincide con el klog, por lo que no demuestra que ese log causara aquellos errores. El runtime escribe sin búfer en `/download0/prospero-radio.log` y solo evalúa el límite de 2 MiB al iniciar. Diagnóstico y plan de confirmación en [`DIAGNOSTICO-RENDIMIENTO-2026-09-28.md`](DIAGNOSTICO-RENDIMIENTO-2026-09-28.md). Corregir la fuente/estilo que origina los avisos y acotar la rotación durante la sesión conservando los avisos útiles; no ocultarlos.

### P1 — Evitar duplicados al volver a importar una M3U

**Complejidad media · riesgo: crecimiento repetido del catálogo AUX y resultados confusos.** El usuario reporta que reenviar una lista con emisoras ya importadas vuelve a agregarlas. Hacer la importación idempotente usando la URL de stream normalizada como identidad; no fusionar solo por nombre, porque una emisora puede publicar URLs distintas (bitrate o formato). Conservar todas las nuevas entradas únicas y comprobar que dos importaciones idénticas no cambien el número de estaciones.

### P1 — Corregir las etiquetas de las fuentes en el LCD

**Complejidad media · riesgo: no identificar la lista activa o seleccionar otra por error.** Las capturas v042 muestran el rótulo central vacío en una vista y `MEM / FAVORITOS` solapado con `AUX M3U` en otra. Revisar ancho, posición, texto real de cada fuente, foco/selección y sincronía con el modelo; probar RADIO, FAVORITOS y AUX M3U con distintos largos y sin datos.

### P2 — Mejorar contraste del encabezado web AUX

**Complejidad baja · riesgo: subtítulo superior ilegible en pantallas pequeñas o con brillo bajo.** En la captura del panel de carga, el texto de modelo situado a la derecha pierde contraste con el metal. Ajustar su color/contraste sin cambiar el estilo de panel trasero aprobado.

### P2 — Detección física del jack del DualSense

**Complejidad alta · API sin verificar.** La pantalla v042 aún muestra `JACK N/A` con el mando presente. La búsqueda dinámica del símbolo es tentativa; no afirmar detección física hasta obtener transiciones reproducibles al conectar y desconectar los auriculares en PS5.

### P2 — Revisar restos del menú de ajustes/temas

El usuario descartó reintroducir ajustes y temas. La v042 no los presenta en los botones principales, pero el árbol conserva elementos ocultos de settings y copia recursos de temas. En una limpieza de código futura, comprobar su alcance antes de retirarlos para no romper el build; no son una función aprobada para la interfaz actual.

## Validaciones recientes

- **Favoritos y presets AUX:** el usuario indica que el problema parece corregido y que ahora funciona. Retirado de pendientes a la espera de una regresión concreta.
- **Cierre del payload y servidor AUX:** el klog de v042 registra `AUX server stopped`, `bridge shutdown complete`, `process exit result=0` y no muestra el módulo del bridge en la muestra FMEM posterior a la salida. Esto valida el cierre en esa ejecución; `download0_rw=FAIL` sigue siendo un resultado explícito del sondeo directo del payload y no debe confundirse con el acceso mediante la API puente. La persistencia de los datos después de reiniciar no queda demostrada por este klog.

## Diferencias registradas

- **01.000.039:** build mostrada en las capturas del usuario; lista/textos, ajustes y señalización de presets presentaron los síntomas anteriores.
- **01.000.041:** candidata documentada para selector de fichero M3U y cambio de fuente en RADIO; no hay paquete v041 ni resultado de consola en el `out/` actual.
- **01.000.042:** capturas en consola confirman el sello de versión, emisora y listas visibles. En esas capturas se observan las pestañas defectuosas y `JACK N/A`; el usuario reporta duplicados tras reimportar una M3U y lentitud tras horas de reproducción. Un klog informa errores `filesystem full`; un log de runtime de otra hora contiene avisos de fuente repetidos. No hay correlación temporal que confirme la causa. El gate local pasó, pero la validación de hardware es parcial.

Registrar por separado código, paquete, hash, gate, klog y observación en PS5. La etiqueta en pantalla identifica la versión mostrada, no el hash instalado; el gate no demuestra audio, acceso LAN ni persistencia en `/data`.
