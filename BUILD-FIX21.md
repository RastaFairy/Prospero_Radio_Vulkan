# BUILD-FIX21 — NIR intrinsic table final-link fix

## Build evidence that triggered FIX21

The real WSL2 build reached the final title link with PSBC and the driver already successful:

- PSBC: 467 sources compiled, **0 compiler warnings**.
- `libpsbc.ps5.a`: installed successfully.
- PS5 Vulkan driver: built successfully.
- `libvulkan.so.1` and `libvulkan-abs.so`: produced successfully.
- Vulkan duplicate-symbol audit: clean.
- Final package conversion stopped on:

```text
error: no public SDK stub exports required symbol nir_intrinsic_infos
```

## Root cause

`nir_intrinsic_infos` is Mesa NIR generated metadata, defined by `src/compiler/nir/nir_intrinsics.c`. It is not a PlayStation system import and must be resolved from the static Mesa/PSBC compiler inputs.

The title linker intentionally permits unresolved symbols so public Sony import stubs can be supplied to the native converter later. That policy allowed this Mesa internal symbol to survive the ELF link until `ps5-native-tool` rejected it as though it were an SDK import.

## FIX21

### `overlay/apply-vulkan.py`

The Vulkan static link input block now adds:

```text
--undefined=nir_intrinsic_infos
```

before the Vulkan archives, forcing LLD to materialize the archive member that provides the generated NIR intrinsic table. A post-link audit now fails early on any remaining `nir_*` undefined symbol.

### `overlay/setup-ps5-vulkan.sh`

The generated NIR intrinsic files are materialized explicitly during the PS5_Vulkan bootstrap:

- `src/compiler/nir/nir_intrinsics.h`
- `src/compiler/nir/nir_intrinsics.c`
- `src/compiler/nir/nir_intrinsics_indices.h`

After PSBC build, the setup script verifies `libpsbc.ps5.a` defines `nir_intrinsic_infos`. After the driver build, it verifies the renamed `libpsbc_driver.ps5.a` still defines the same symbol.

These checks make the failure local and diagnostic instead of deferring it to the title converter.

## Validation

- `python3 -m py_compile overlay/apply-vulkan.py`: passed.
- `bash -n overlay/setup-ps5-vulkan.sh`: passed.
- Structural patch test for the Vulkan archive block: passed.
- Existing FIX20 reset/warning-policy logic retained.

## Expected next build

The next WSL2 run should either:

1. resolve `nir_intrinsic_infos` and continue to title conversion/package generation, or
2. fail earlier with an explicit PSBC/driver-archive diagnostic if the symbol is genuinely absent from the compiler archives.

No final package success is claimed until the real build produces and validates `eboot.bin`, FFPKG and FFPFSC.
