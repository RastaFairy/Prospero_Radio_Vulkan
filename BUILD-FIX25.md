# FIX25

## Failure fixed

FIX24 failed before dependency/build stages because `overlay/setup-ps5-vulkan.sh`
contained an embedded Python dictionary using nested triple-quoted strings. The
outer heredoc Python parser terminated the string early and produced:

    SyntaxError: invalid syntax
    "NIR_SRCS": """NIR_GENERATED_SRCS = \\

## Change

The generated-source patch no longer uses nested multiline Python string literals.
It appends the generated C sources directly to the existing GNU Make variables,
one line per source, and checks for each line before insertion so the patch is
idempotent.

NIR sources:
- src/compiler/nir/nir_constant_expressions.c
- src/compiler/nir/nir_intrinsics.c
- src/compiler/nir/nir_opcodes.c
- src/compiler/nir/nir_opt_algebraic.c

SPIR-V sources:
- src/compiler/spirv/spirv_info.c
- src/compiler/spirv/vtn_gather_types.c

## Validation

- `bash -n overlay/setup-ps5-vulkan.sh`
- extracted embedded Python passes `ast.parse`
- synthetic Makefile patch executes successfully
- applying the synthetic patch twice produces identical output
- `python3 -m compileall -q overlay`
- ZIP integrity validated with `unzip -t`
