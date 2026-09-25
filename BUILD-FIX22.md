# BUILD-FIX23

## Result from FIX21
The build reached the final title link. PSBC compiled with 0 warnings and the
PS5 driver built successfully. The final audit reported these Mesa NIR symbols:

- nir_eval_const_opcode
- nir_op_infos
- nir_opt_algebraic
- nir_opt_algebraic_late
- nir_opt_reassociate_for_fma
- nir_type_conversion_op

`nir_intrinsic_infos` was already forced by FIX21 and disappeared from the
reported unresolved set, confirming that archive-member extraction is the
issue rather than missing SDK stubs.

## FIX23
The title linker patch now forces one `--undefined=` anchor for each generated
NIR core member represented by those symbols, plus `nir_intrinsic_infos`:

- `nir_intrinsics.c`
- `nir_opcodes.c`
- `nir_constant_expressions.c`
- `nir_opt_algebraic.c`

The existing `--whole-archive` treatment remains in place. This does not create
stubs or fake imports; it causes LLD to extract the real Mesa NIR definitions
from `libpsbc_driver.ps5.a`.


FIX23 change: the post-link Vulkan/NIR unresolved-symbol audit now inspects the ordinary ELF symbol table with `prospero-nm --undefined-only`, not the dynamic table (`-D`). This prevents internal/static Mesa NIR definitions from being misclassified as runtime imports. Weak undefined Vulkan entry points remain intentionally ignored.


### FIX24 root-cause correction
The prior title-link audit correctly showed six unresolved NIR symbols, but the attempted link-only fixes did not address why those definitions were absent from the PSBC archive. The pinned opengnm-psbc Makefile uses `$(wildcard src/compiler/nir/*.c)` and `$(wildcard src/compiler/spirv/*.c)` while generated `.c` files are materialized as Make targets. Because wildcard expansion occurs when the Makefile is parsed, generated implementations can be omitted from the source lists. FIX24 explicitly adds the generated NIR/SPIR-V C sources to the corresponding source variables after the standalone bootstrap materializes them, and verifies all seven generated NIR core symbols are present in `libpsbc.ps5.a`.
