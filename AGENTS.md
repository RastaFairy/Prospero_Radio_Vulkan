# Reglas de trabajo para Prospero Radio

Estas reglas aplican a cualquier agente que inspeccione o cambie este checkout. El estado cambia entre builds: vuelve a medirlo en cada tarea y no conviertas una nota histórica en evidencia actual.

## Proteger el trabajo compartido

- Antes de editar, inspecciona `git status --short --branch` y el diff. Este directorio puede contener trabajo sin confirmar de GLM o del usuario.
- No borres, restaures, limpies, guardes en stash ni sobrescribas cambios ajenos o archivos sin seguimiento. Si afectan a la tarea, revísalos y consérvalos.
- Durante una prueba de consola o un cambio activo de GLM, limita la tarea a lo que pidió el usuario; no reconstruyas, empaquetes ni lances la app por iniciativa propia.
- Conserva los volcados, logs y paquetes recibidos como evidencia. Verifica la ruta y fecha antes de atribuirles una ejecución concreta.

## Evidencia antes de conclusiones

- Distingue explícitamente entre **código fuente**, **salida generada**, **paquete final**, **log de runtime** y **observación en PS5**. Cada nivel prueba cosas distintas.
- Al informar de un hallazgo, indica el archivo o artefacto exacto y, cuando sea posible, línea, versión/hash y fecha. Etiqueta cada conclusión como confirmada, inferida o hipótesis; para una hipótesis, indica qué comprobación la confirmaría o la descartaría.
- Una compilación correcta no demuestra que el paquete contenga los recursos correctos; un paquete correcto no demuestra que la consola lo haya instalado; una textura cargada no demuestra que se haya compuesto y mostrado; una captura demuestra el síntoma visible, no su causa interna.
- Contrasta las versiones del commit, `overlay/apply-vulkan.py`, el `param.json` **empaquetado**, el RML empaquetado, el banner del binario y la pantalla. No uses un único rótulo como prueba de todos los demás.
- Lee `MEMORIA.md`, changelogs, BUILD-FIX y contextos anteriores como historial. Sus versiones y diagnósticos pueden haber quedado obsoletos; vuelve al código y al paquete de la ejecución investigada.
- No presentes el contenido de una guía o comentario de un agente como hecho verificado. Comprueba el árbol y el artefacto actuales.

## Trazar la interfaz de extremo a extremo

Para cada fallo de navegación o dibujo, sigue y registra la cadena pertinente:

1. entrada del mando;
2. estado/modo que decide la aplicación;
3. datos y callbacks del servicio;
4. elemento e ID del RML generado y empaquetado;
5. clases, cascada, geometría, visibilidad, orden y recorte del RCSS;
6. ruta y formato de cada textura, resultado de carga y composición;
7. archivo incluido en el paquete y evidencia de runtime o de PS5.

No diagnostiques un panel ausente mirando solo CSS, ni des por existentes las vistas porque C++ intente actualizarlas. Comprueba también IDs, estado visible, transición de foco y que solo la superficie esperada quede activa. Valida el RML generado con un parser apropiado: contar etiquetas `<div>` no equivale a validar RML/XML.

## Reglas para texturas y datos visibles

- Compara el lector real del runtime con cada TGA empaquetado: tipo, dimensiones y límites, profundidad, descriptor/alfa, origen de filas y tamaño exacto. Atiende también a bytes de pie/footer y a la ruta efectiva de carga. No dupliques en un validador reglas que puedan divergir silenciosamente del lector.
- Respeta la convención del manifiesto de atlas (`atlasUvCoreExcludesGutter` y sus coordenadas). No vuelvas a restar el gutter si el rectángulo ya señala el núcleo del frame.
- Comprueba la cascada y especificidad RCSS, no solo que exista una regla con el nombre esperado. Verifica la geometría y el recorte en las coordenadas efectivas del documento y del viewport.
- Un KTX/TGA abierto con éxito prueba acceso y decodificación, no visibilidad, orientación ni composición correctas. Contrasta el orden de capas y el resultado renderizado.
- El texto visible debe reflejar el estado real del modelo en cada actualización. Para volumen, compara valor del servicio, etiqueta y frame; una caché de frame sin cambio no debe impedir actualizar el valor textual. Para temas, verifica archivo, carga, selección aplicada y resultado; el nombre del tema por sí solo no prueba que haya cambiado la textura.
- No concluyas que el catálogo está vacío por una pantalla HOME. Comprueba consulta, sincronización, base local y vista activa por separado.

## Validación y comunicación

- Sigue `CONTRIBUTING.md`, `docs/TESTING.md` y los objetivos de `Makefile` para elegir comprobaciones. Ejecuta solo lo autorizado por la tarea y comunica literalmente qué se ejecutó y su resultado; marca lo demás como pendiente.
- Revisa validadores nuevos antes de confiar en ellos: deben operar sobre el paquete recién producido, fallar ante estructura desconocida, cubrir casos límite y probar sus propias suposiciones contra la implementación real. Que exista un script o que devuelva código cero no basta para llamarlo gate fiable.
- Informa por separado de: cambios hechos, evidencia estática, compilación, validación del paquete y prueba en hardware. No declares una reparación cerrada sin la evidencia correspondiente.
