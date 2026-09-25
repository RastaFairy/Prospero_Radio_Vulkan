# ProsperoRadio Modern — BUILD-FIX15

## FIX15 — corrección de la auditoría post-link Vulkan

La ejecución real de FIX14 confirmó:

```text
Vulkan link symbol audit: no strong duplicate symbols
```

y después la nueva auditoría informó decenas de `vk*` indefinidos. La revisión del comportamiento de Mesa/PS5_Vulkan muestra que el runtime genera deliberadamente entry points opcionales como símbolos **weak undefined**. No son imports fuertes del título y no deben tratarse como errores de enlace.

FIX15 cambia exclusivamente la auditoría post-link para usar `prospero-nm --format=posix` y aceptar únicamente símbolos con tipo `U` (undefined fuerte):

```bash
prospero-nm -D --undefined-only --format=posix build/llvm-pie.elf |
    awk '$2 == "U" && $1 ~ /^vk[A-Z]/ { print $1 }'
```

Los símbolos con tipo `w` permanecen permitidos, como exige el diseño de dispatch de Mesa. Un Vulkan fuerte realmente sin resolver seguirá deteniendo el build y se mostrará completo.

## Confirmación de la causa

La integración oficial de PS5_Vulkan enlaza `libps5vk.ps5.a` y `libvk_runtime.ps5.a` con `--whole-archive` y usa `-z nodynamic-undefined-weak`; además, el runtime de Mesa documenta entry points weak opcionales. Por tanto, la lista anterior no demuestra por sí sola que el renderer tenga símbolos fuertes sin resolver: el auditor FIX14 estaba clasificando ambos casos como errores.

## Validación FIX15

- `python3 -m py_compile overlay/apply-vulkan.py` — PASS.
- `nm --format=posix` sintético: `vkStrong U` detectado, `vkWeak w` ignorado — PASS.
- `awk` del filtro post-link probado aisladamente — PASS.
- El auditor previo de símbolos duplicados permanece activo.
- Se conserva la eliminación de los 44 warnings del renderer realizada en FIX14.

## Estado

El próximo build debe superar la auditoría post-link sin considerar los weak `vk*` como errores. Después, el conversor FSELF será quien determine si existe algún import fuerte Vulkan realmente no proporcionado. No se declara éxito final hasta generar y validar `eboot.bin` y los paquetes.
