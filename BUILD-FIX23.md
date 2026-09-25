# BUILD-FIX23

## Result from FIX22
FIX22 added `--undefined=` anchors in `apply-vulkan.py` (inside
`patch_target_build_driver`) for these six NIR symbols plus
`nir_intrinsic_infos`:

- nir_eval_const_opcode
- nir_op_infos
- nir_opt_algebraic
- nir_opt_algebraic_late
- nir_opt_reassociate_for_fma
- nir_type_conversion_op

The build still fails with the same Mesa link audit because the anchors
reference symbols that are **absent** from `libpsbc_driver.ps5.a`.
`--undefined=SYM` can only pull a member out of an archive when that
member is already present; it cannot invent a definition. The six symbols
originate from Mesa-generated `.c` files that the PSBC bootstrap in
`patch_ps5_vulkan_psbc_bootstrap()` never materialized:

| Symbol(s)                                     | Generated source               | Generator script                              |
|-----------------------------------------------|-------------------------------|-----------------------------------------------|
| `nir_op_infos`                                | `nir_opcodes.c`               | `nir_opcodes_c.py`                            |
| `nir_eval_const_opcode`, `nir_type_conversion_op` | `nir_constant_expressions.c` | `nir_constant_expressions.py`                 |
| `nir_opt_algebraic`, `nir_opt_algebraic_late`, `nir_opt_reassociate_for_fma` | `nir_opt_algebraic.c` | `nir_opt_algebraic.py` |

## FIX23

`setup-ps5-vulkan.sh` / `patch_ps5_vulkan_psbc_bootstrap` now generates
all four source files before the PSBC archive is assembled:

```bash
python3 src/compiler/nir/nir_opcodes_h.py     --out src/compiler/nir/nir_opcodes.h
python3 src/compiler/nir/nir_opcodes_c.py     --out src/compiler/nir/nir_opcodes.c
python3 src/compiler/nir/nir_constant_expressions.py > src/compiler/nir/nir_constant_expressions.c
python3 src/compiler/nir/nir_opt_algebraic.py > src/compiler/nir/nir_opt_algebraic.c
```

`nir_opcodes.h` is generated first because `nir_opcodes.c` and
`nir_constant_expressions.c` both include it.

The `--undefined=` anchors already present in `apply-vulkan.py` (FIX22)
remain correct and now find their definitions in the archive.

Both NIR archive checks (PSBC base archive after `build-psbc-ps5.sh` and
the driver archive after `build-driver.sh`) now validate all seven symbols
and report which generated source is missing, rather than only checking
`nir_intrinsic_infos`.

## No changes to apply-vulkan.py

`apply-vulkan.py` is unchanged from FIX22. The `--undefined=` anchors it
inserts are still correct; they were always the right mechanism. The root
cause was upstream: the archive entries those anchors pointed at did not
exist.
