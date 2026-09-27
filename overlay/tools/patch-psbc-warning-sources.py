#!/usr/bin/env python3
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply checked, source-level warning fixes to the pinned PSBC work copy."""
from __future__ import annotations

import re
import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count == 1:
        return text.replace(old, new, 1)
    if count == 0 and new in text:
        return text
    raise RuntimeError(f"{label}: expected one source match, found {count}")


def replace_count(text: str, old: str, new: str, expected: int, label: str) -> str:
    count = text.count(old)
    if count == expected:
        return text.replace(old, new)
    if count == 0 and text.count(new) == expected:
        return text
    raise RuntimeError(f"{label}: expected {expected} source matches, found {count}")


def patch_nir_pass(tree: Path) -> int:
    header = tree / "src/compiler/nir/nir.h"
    text = header.read_text(encoding="utf-8")
    name = "#define NIR_PASS(progress, nir, pass, ...)"
    start = text.find(name)
    if start < 0:
        raise RuntimeError("NIR_PASS definition not found in nir.h")
    end_marker = "\n})"
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError("NIR_PASS macro terminator not found in nir.h")
    end += len(end_marker)
    macro = text[start:end]
    macro, dummy_count = re.subn(
        r"^[ \t]*UNUSED bool _;[ \t]*\\\n",
        "",
        macro,
        count=1,
        flags=re.MULTILINE,
    )
    if dummy_count == 1:
        # The temporary is never read. Removing it keeps the pass semantics
        # intact and prevents a real -Wunused-variable diagnostic.
        text = text[:start] + macro + text[end:]
        end = start + len(macro)
    elif "UNUSED bool _;" in macro:
        raise RuntimeError("NIR_PASS dummy variable changed unexpectedly")
    if macro.count("      progress = true;") != 1:
        raise RuntimeError("NIR_PASS progress assignment changed unexpectedly")

    no_progress_name = "#define NIR_PASS_NO_PROGRESS(nir, pass, ...)"
    if no_progress_name not in text:
        ignored = macro.replace(name, no_progress_name, 1)
        ignored, progress_count = re.subn(
            r"^[ \t]*progress = true;[ \t]*\\\n",
            "",
            ignored,
            count=1,
            flags=re.MULTILINE,
        )
        if progress_count != 1:
            raise RuntimeError("NIR_PASS progress assignment could not be cloned")
        text = text[:end] + "\n\n" + ignored + text[end:]
    header.write_text(text, encoding="utf-8")

    call_sites = 0
    for path in tree.rglob("*"):
        if path.suffix not in {".c", ".h", ".cpp", ".hpp"}:
            continue
        source = path.read_text(encoding="utf-8", errors="strict")
        fixed, count = re.subn(r"\bNIR_PASS\(\s*_,\s*", "NIR_PASS_NO_PROGRESS(", source)
        call_sites += count
        if count:
            path.write_text(fixed, encoding="utf-8")
    remaining = 0
    for path in tree.rglob("*"):
        if path.suffix in {".c", ".h", ".cpp", ".hpp"}:
            remaining += len(re.findall(r"\bNIR_PASS\(\s*_\s*,", path.read_text(encoding="utf-8")))
    if remaining:
        raise RuntimeError(f"{remaining} ignored-output NIR_PASS call(s) remain")
    return call_sites


def patch_math_generic_calls(tree: Path) -> int:
    # The PS5 SDK's C11 math macros use _Generic associations for qualified
    # float types that lvalue conversion makes unreachable. Compiler builtins
    # have the same numeric semantics without expanding those dead branches.
    pattern = re.compile(
        r"(?<![A-Za-z0-9_])(isnan|isinf|isfinite|isnormal|signbit)(?=\s*\()"
    )
    total = 0
    generator = tree / "src/compiler/nir/nir_opcodes.py"
    generator_source = generator.read_text(encoding="utf-8", errors="strict")
    generator_fixed, generator_count = pattern.subn(r"__builtin_\1", generator_source)
    if generator_count:
        generator.write_text(generator_fixed, encoding="utf-8")
        total += generator_count

    for path in tree.rglob("*"):
        if path.suffix not in {".c", ".h", ".cpp", ".hpp"}:
            continue
        source = path.read_text(encoding="utf-8", errors="strict")
        fixed, count = pattern.subn(r"__builtin_\1", source)
        if count:
            path.write_text(fixed, encoding="utf-8")
            total += count
    for path in tree.rglob("*"):
        if path.suffix in {".c", ".h", ".cpp", ".hpp"}:
            if pattern.search(path.read_text(encoding="utf-8", errors="strict")):
                raise RuntimeError(f"unfixed qualified math generic in {path}")
    if not total and not (
        "__builtin_isnan(" in generator.read_text(encoding="utf-8", errors="strict")
        or any(
            "__builtin_isnan(" in path.read_text(encoding="utf-8", errors="strict")
            for path in tree.rglob("*.c")
        )
    ):
        raise RuntimeError("math generic warning sites not found in PSBC sources")
    return total


def patch_optional_generated_helper(tree: Path) -> None:
    files = [
        tree / "src/compiler/nir/nir_constant_expressions.py",
        tree / "src/compiler/nir/nir_constant_expressions.c",
    ]
    for path in files:
        if not path.is_file():
            continue
        source = path.read_text(encoding="utf-8", errors="strict")
        source = replace_once(
            source,
            "static inline uint8_t _mesa_half_to_snorm8(uint16_t val)",
            "static UNUSED inline uint8_t _mesa_half_to_snorm8(uint16_t val)",
            "optional generated half-to-snorm8 helper annotation",
        )
        path.write_text(source, encoding="utf-8")


def patch_spirv_fail(tree: Path) -> None:
    path = tree / "src/compiler/spirv/vtn_private.h"
    text = path.read_text(encoding="utf-8")
    old = "#define vtn_fail(...) _vtn_fail(b, __FILE__, __LINE__, __VA_ARGS__)"
    new = (
        "#define vtn_fail(...) \\\n"
        "   do { \\\n"
        "      _vtn_fail(b, __FILE__, __LINE__, __VA_ARGS__); \\\n"
        "      __builtin_unreachable(); \\\n"
        "   } while (0)"
    )
    path.write_text(replace_once(text, old, new, "SPIR-V fail contract"), encoding="utf-8")


def patch_nir_print(tree: Path) -> None:
    path = tree / "src/compiler/nir/nir_print.c"
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "nir_variable_mode mode, char *buf)",
        "nir_variable_mode mode, char *buf, size_t buf_size)",
        "NIR location formatter signature",
    )
    text = replace_once(
        text,
        'snprintf(buf, 4, "%u", location);',
        'snprintf(buf, buf_size, "%u", location);',
        "NIR location formatter capacity",
    )
    text = replace_count(text, "char buf[4];", "char buf[11];", 2, "NIR location buffer size")
    text = replace_once(
        text,
        "var->data.mode, buf);",
        "var->data.mode, buf, sizeof(buf));",
        "NIR variable location buffer argument",
    )
    text = replace_once(
        text,
        "state->shader->info.stage, mode,\n                                            buf);",
        "state->shader->info.stage, mode,\n                                            buf, sizeof(buf));",
        "NIR IO location buffer argument",
    )
    path.write_text(text, encoding="utf-8")


def patch_miscellaneous(tree: Path) -> None:
    hash_table = tree / "src/util/hash_table.c"
    text = hash_table.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "static inline void *\nuint_key(unsigned id)",
        "/* Retained integer-key encoder for clients that store unsigned IDs as pointer keys. */\n"
        "static UNUSED inline void *\nuint_key(unsigned id)",
        "retained optional unsigned-key encoder annotation",
    )
    hash_table.write_text(text, encoding="utf-8")

    bptc = tree / "src/util/format/texcompress_bptc_tmp.h"
    text = bptc.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "static void\ndecompress_rgb_fp16(int width, int height,",
        "/* Retained half-float BC6H decoder for formats without a current Mesa entrypoint. */\n"
        "static UNUSED void\n"
        "decompress_rgb_fp16(int width, int height,",
        "retained optional BC6H fp16 decoder annotation",
    )
    bptc.write_text(text, encoding="utf-8")

    threads = tree / "src/c11/impl/threads_posix.c"
    text = threads.read_text(encoding="utf-8")
    timedlock_helper = "static int\nthreads_timespec_compare(const struct timespec *a, const struct timespec *b)"
    guarded_timedlock_helper = "#ifndef EMULATED_THREADS_USE_NATIVE_TIMEDLOCK\n" + timedlock_helper
    if guarded_timedlock_helper not in text:
        text = replace_once(
            text,
            timedlock_helper,
            guarded_timedlock_helper,
            "POSIX emulated timed-lock helper feature guard",
        )
    text = replace_once(
        text,
        "    return 0;\n}\n\n// 7.25.4.4",
        "    return 0;\n}\n#endif /* !EMULATED_THREADS_USE_NATIVE_TIMEDLOCK */\n\n// 7.25.4.4",
        "POSIX emulated timed-lock helper feature guard end",
    )
    threads.write_text(text, encoding="utf-8")

    blake3_header = tree / "src/util/blake3/blake3_impl.h"
    text = blake3_header.read_text(encoding="utf-8")
    blake3_prototype = (
        "static size_t blake3_compress_subtree_wide(const uint8_t *input, size_t input_len,\n"
        "                                           const uint32_t key[8],\n"
        "                                           uint64_t chunk_counter, uint8_t flags,\n"
        "                                           uint8_t *out, bool use_tbb);\n"
    )
    if text.count(blake3_prototype) == 1:
        text = text.replace(blake3_prototype, "", 1)
    elif text.count(blake3_prototype) != 0:
        raise RuntimeError("BLAKE3 private declaration changed unexpectedly")
    blake3_header.write_text(text, encoding="utf-8")

    clear_copy = tree / "src/amd/common/nir/ac_nir_meta_cs_clear_copy_buffer.c"
    text = clear_copy.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "nir_channels(&b, value, BITFIELD_MASK(num_dwords))",
        "nir_channels(&b, value, (nir_component_mask_t)BITFIELD_MASK(num_dwords))",
        "bounded NIR component mask",
    )
    clear_copy.write_text(text, encoding="utf-8")

    instance = tree / "src/amd/vulkan/radv_instance.h"
    text = instance.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "struct radv_instance {",
        "struct radv_physical_device;\n\nstruct radv_instance {",
        "RADV physical-device forward declaration",
    )
    instance.write_text(text, encoding="utf-8")

    aco = tree / "src/amd/compiler/instruction_selection/aco_select_nir_intrinsics.cpp"
    text = aco.read_text(encoding="utf-8")
    text = replace_count(
        text,
        "ffs(const_offset) > req_lsb_zero",
        "ffs(const_offset) > static_cast<int>(req_lsb_zero)",
        2,
        "ACO signed alignment comparison",
    )
    aco.write_text(text, encoding="utf-8")

    instr_set = tree / "src/compiler/nir/nir_instr_set.c"
    text = instr_set.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "if (src1->pred == src2->pred)",
        "if (cmp_phi_src(&src1, &src2) == 0)",
        "NIR phi predecessor comparator use",
    )
    instr_set.write_text(text, encoding="utf-8")

    workgroup = tree / "src/compiler/nir/nir_lower_workgroup_size.c"
    text = workgroup.read_text(encoding="utf-8")
    orphan_prototype = "static bool nlwgs_cf_list_has_barrier(struct exec_list *cf_list);\n"
    if text.count(orphan_prototype) == 1:
        text = text.replace(orphan_prototype, "", 1)
    elif text.count(orphan_prototype) != 0 or "nlwgs_cf_list_has_barrier" in text:
        raise RuntimeError("orphan workgroup barrier prototype changed unexpectedly")
    workgroup.write_text(text, encoding="utf-8")

    varyings = tree / "src/compiler/nir/nir_opt_varyings.c"
    text = varyings.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "static void\nprint_shader_linkage(",
        "static UNUSED void\nprint_shader_linkage(",
        "retained NIR linkage diagnostic helper",
    )
    varyings.write_text(text, encoding="utf-8")

    lower_to_hw = tree / "src/amd/compiler/aco_lower_to_hw_instr.cpp"
    text = lower_to_hw.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "               if (linear_vgpr)\n"
        "                  handle_operands_linear_vgpr(copy_operations, &ctx, program->gfx_level, pi);",
        "               if (linear_vgpr && !non_linear_vgpr)\n"
        "                  handle_operands_linear_vgpr(copy_operations, &ctx, program->gfx_level, pi);",
        "ACO parallel-copy class fallback",
    )
    lower_to_hw.write_text(text, encoding="utf-8")

    spill_preserved = tree / "src/amd/compiler/aco_spill_preserved.cpp"
    text = spill_preserved.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '      assert(found_reg && "aco/spill_preserved: No free space to store exec mask backup!");',
        '      if (!found_reg)\n'
        '         UNREACHABLE("aco/spill_preserved: No free space to store exec mask backup!");',
        "ACO preserved-register backup exhaustion handling",
    )
    spill_preserved.write_text(text, encoding="utf-8")

    nir_intrinsics = tree / "src/amd/compiler/instruction_selection/aco_select_nir_intrinsics.cpp"
    text = nir_intrinsics.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "   unsigned align = nir_intrinsic_align_mul(instr) ? nir_intrinsic_align(instr) : elem_size_bytes;",
        "   ASSERTED unsigned align = nir_intrinsic_align_mul(instr) ? nir_intrinsic_align(instr) : elem_size_bytes;",
        "ACO shared-load assert-only alignment",
    )
    nir_intrinsics.write_text(text, encoding="utf-8")


def patch_sample_mask_helper_use(tree: Path) -> None:
    """Keep and use the shared initial-helper query in the sample-mask pass."""
    early = tree / "src/amd/common/nir/ac_nir_lower_ps_early.c"
    text = early.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "   nir_def *load_helper_invoc_at_top;\n",
        "",
        "unused early-pass helper cache field",
    )
    old_helper = (
        "static nir_def *\n"
        "get_load_helper_invocation(nir_function_impl *impl, lower_ps_early_state *s)\n"
        "{\n"
        "   /* Insert this only once. */\n"
        "   if (!s->load_helper_invoc_at_top) {\n"
        "      nir_builder b = nir_builder_at(nir_before_impl(impl));\n"
        "      s->load_helper_invoc_at_top = nir_load_helper_invocation(&b, 1);\n"
        "   }\n"
        "\n"
        "   return s->load_helper_invoc_at_top;\n"
        "}\n"
    )
    text = replace_once(text, old_helper, "", "unreferenced early-pass helper")
    early.write_text(text, encoding="utf-8")

    sample_mask = tree / "src/amd/common/nir/ac_nir_lower_sample_mask_in.c"
    text = sample_mask.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '#include "nir_builder.h"\n\n',
        '#include "nir_builder.h"\n\n'
        "typedef struct {\n"
        "   const ac_nir_lower_sample_mask_in_options *options;\n"
        "   nir_def *load_helper_invoc_at_top;\n"
        "} ac_nir_lower_sample_mask_in_state;\n\n",
        "sample-mask helper state",
    )
    old_helper = (
        "static nir_def *\n"
        "get_initial_helper_invocation(nir_builder *b)\n"
        "{\n"
        "   nir_builder top = nir_builder_at(nir_before_impl(b->impl));\n"
        "   return nir_load_helper_invocation(&top, 1);\n"
        "}"
    )
    new_helper = (
        "static nir_def *\n"
        "get_load_helper_invocation(nir_builder *b, ac_nir_lower_sample_mask_in_state *s)\n"
        "{\n"
        "   /* Insert this only once per shader, before divergent control flow. */\n"
        "   if (!s->load_helper_invoc_at_top) {\n"
        "      nir_builder top = nir_builder_at(nir_before_impl(b->impl));\n"
        "      s->load_helper_invoc_at_top = nir_load_helper_invocation(&top, 1);\n"
        "   }\n"
        "   return s->load_helper_invoc_at_top;\n"
        "}"
    )
    text = replace_once(text, old_helper, new_helper, "sample-mask helper cache implementation")
    text = replace_once(
        text,
        "get_sample_mask_for_1sample(nir_builder *b)",
        "get_sample_mask_for_1sample(nir_builder *b, ac_nir_lower_sample_mask_in_state *s)",
        "sample-mask helper cache argument",
    )
    text = replace_once(
        text,
        "nir_inot(b, get_initial_helper_invocation(b))",
        "nir_inot(b, get_load_helper_invocation(b, s))",
        "sample-mask shared helper use",
    )
    text = replace_once(
        text,
        "   const ac_nir_lower_sample_mask_in_options *options =\n"
        "      (const ac_nir_lower_sample_mask_in_options*)data;",
        "   ac_nir_lower_sample_mask_in_state *s = (ac_nir_lower_sample_mask_in_state *)data;\n"
        "   const ac_nir_lower_sample_mask_in_options *options = s->options;",
        "sample-mask callback state",
    )
    text = replace_count(
        text,
        "get_sample_mask_for_1sample(b)",
        "get_sample_mask_for_1sample(b, s)",
        3,
        "sample-mask cached helper call sites",
    )
    text = replace_once(
        text,
        "   return nir_shader_intrinsics_pass(nir, lower_sample_mask_in, nir_metadata_control_flow,\n"
        "                                     (void*)options);",
        "   ac_nir_lower_sample_mask_in_state state = {.options = options};\n"
        "   return nir_shader_intrinsics_pass(nir, lower_sample_mask_in, nir_metadata_control_flow,\n"
        "                                     &state);",
        "sample-mask pass state initialization",
    )
    sample_mask.write_text(text, encoding="utf-8")


def patch_radv_optional_and_shared_helpers(tree: Path) -> None:
    shader_info = tree / "src/amd/vulkan/radv_shader_info.c"
    text = shader_info.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "   if (es_info->stage == MESA_SHADER_TESS_EVAL) {\n"
        "      if (es_info->tes.point_mode)\n"
        "         return MESA_PRIM_POINTS;\n"
        "      if (es_info->tes._primitive_mode == TESS_PRIMITIVE_ISOLINES)\n"
        "         return MESA_PRIM_LINES;\n"
        "      return MESA_PRIM_TRIANGLES;\n"
        "   }\n\n"
        "   return MESA_PRIM_TRIANGLES;",
        "   const unsigned vertices = radv_get_num_input_vertices(es_info, NULL);\n"
        "   if (vertices == 1)\n"
        "      return MESA_PRIM_POINTS;\n"
        "   if (vertices == 2)\n"
        "      return MESA_PRIM_LINES;\n"
        "   return MESA_PRIM_TRIANGLES;",
        "RADV pre-raster topology shared vertex-count helper",
    )
    text = replace_once(
        text,
        "   ac_ngg_compute_subgroup_info(gfx_level, es_info->stage, !!gs_info, input_prim, gs_vertices_out, gs_num_invocations,\n"
        "                                128, 128, stage_info->wave_size, es_info->esgs_itemsize,\n"
        "                                stage_info->ngg_lds_vertex_size, stage_info->ngg_lds_scratch_size, false, 0, &info);\n\n"
        "   out->hw_max_esverts = info.hw_max_esverts;",
        "   ac_ngg_compute_subgroup_info(gfx_level, es_info->stage, !!gs_info, input_prim, gs_vertices_out, gs_num_invocations,\n"
        "                                128, 128, stage_info->wave_size, es_info->esgs_itemsize,\n"
        "                                stage_info->ngg_lds_vertex_size, stage_info->ngg_lds_scratch_size, false, 0, &info);\n"
        "\n"
        "   /* Verify the shared NGG solver respects the primitive reuse limit. */\n"
        "   const unsigned min_verts_per_prim = gs_info ? mesa_vertices_per_prim(input_prim) : 1;\n"
        "   unsigned clamped_gsprims = info.max_gsprims;\n"
        "   clamp_gsprims_to_esverts(&clamped_gsprims, info.hw_max_esverts, min_verts_per_prim,\n"
        "                           mesa_prim_has_adjacency(input_prim));\n"
        "   if (clamped_gsprims != info.max_gsprims)\n"
        "      UNREACHABLE(\"NGG subgroup solver exceeded ES vertex reuse limit\");\n\n"
        "   out->hw_max_esverts = info.hw_max_esverts;",
        "RADV NGG primitive reuse invariant",
    )
    shader_info.write_text(text, encoding="utf-8")

    meta_nir = tree / "src/amd/vulkan/nir/radv_meta_nir.c"
    text = meta_nir.read_text(encoding="utf-8")
    text = replace_once(
        text,
        'nir_builder b = radv_meta_nir_init_shader(MESA_SHADER_COMPUTE, "meta_resolve_cs");',
        'nir_builder b = radv_meta_nir_init_shader(MESA_SHADER_COMPUTE, "meta_resolve_cs_%s_%ds_%s",\n'
        '                                                 radv_meta_resolve_compute_type_name(type), samples,\n'
        '                                                 get_resolve_mode_str(resolve_mode));',
        "RADV resolve shader diagnostic name",
    )
    meta_nir.write_text(text, encoding="utf-8")

    shader = tree / "src/amd/vulkan/radv_shader.c"
    text = shader.read_text(encoding="utf-8")
    runtime_only_helpers = (
        ("radv_precompute_registers_hw_vs", "static void\nradv_precompute_registers_hw_hs("),
        ("radv_precompute_registers_hw_hs", "void\nradv_precompute_registers_hw_gs("),
        ("radv_precompute_registers_hw_ms", "static void\nradv_precompute_registers_hw_fs("),
        ("radv_precompute_registers_hw_fs", "static void\nradv_precompute_registers_hw_cs("),
        ("radv_precompute_registers_hw_cs", "static void\nradv_precompute_registers_pgm("),
        ("radv_precompute_registers_pgm", "static void\nradv_precompute_registers("),
    )
    for name, next_signature in runtime_only_helpers:
        signature = f"static void\n{name}("
        guarded_signature = f"#if !defined(PROSPERO_PSBC_STANDALONE)\n{signature}"
        if guarded_signature in text:
            continue
        start = text.find(signature)
        if start < 0 or text.find(signature, start + len(signature)) >= 0:
            raise RuntimeError(f"{name}: expected one runtime-only definition")
        end = text.find("\n\n" + next_signature, start)
        if end < 0 and name == "radv_precompute_registers_pgm":
            end = text.find(
                "\n\n#if !defined(PROSPERO_PSBC_STANDALONE)\n" + next_signature,
                start,
            )
        if end < 0:
            raise RuntimeError(f"{name}: next-function boundary changed")
        body = text[start:end]
        text = (
            text[:start]
            + "#if !defined(PROSPERO_PSBC_STANDALONE)\n"
            + body
            + "\n#endif /* !PROSPERO_PSBC_STANDALONE */"
            + text[end:]
        )
    aggregator = "static void\nradv_precompute_registers(struct radv_device *device, struct radv_shader *shader)"
    guarded_aggregator = "#if !defined(PROSPERO_PSBC_STANDALONE)\n" + aggregator
    if guarded_aggregator not in text:
        text = replace_once(
            text,
            aggregator,
            guarded_aggregator,
            "full-driver register precompute feature guard",
        )
    text = replace_once(
        text,
        "}\n\nstatic bool\nradv_mem_ordered(enum amd_gfx_level gfx_level)",
        "}\n#endif /* !PROSPERO_PSBC_STANDALONE */\n\n"
        "static bool\n"
        "radv_mem_ordered(enum amd_gfx_level gfx_level)",
        "full-driver register precompute feature guard end",
    )
    text = replace_once(
        text,
        "static void\nradv_fill_llvm_compiler_options(",
        "#if AMD_LLVM_AVAILABLE\n"
        "static void\n"
        "radv_fill_llvm_compiler_options(",
        "RADV optional LLVM helper guard",
    )
    text = replace_once(
        text,
        "   options->check_ir = compiler_info->debug.check_ir;\n}\n\nstatic inline void\nradv_aco_fill_compiler_options(",
        "   options->check_ir = compiler_info->debug.check_ir;\n}\n"
        "#endif /* AMD_LLVM_AVAILABLE */\n\n"
        "static inline void\n"
        "radv_aco_fill_compiler_options(",
        "RADV optional LLVM helper guard end",
    )
    text = replace_once(
        text,
        "static uint64_t\nradv_dump_flag_for_stage(const mesa_shader_stage stage)",
        "/* Legacy RADV_DEBUG_DUMP bit mapping retained for non-Vk debug consumers. */\n"
        "static UNUSED uint64_t\n"
        "radv_dump_flag_for_stage(const mesa_shader_stage stage)",
        "retained legacy RADV dump-flag mapping",
    )
    shader.write_text(text, encoding="utf-8")


def patch_optional_rgp_serialization(tree: Path) -> None:
    """Compile RGP serialization helpers only with their USE_LIBELF consumer.

    The PS5 configuration deliberately builds the no-libelf capture stub. The
    serializers are still needed by libelf-enabled Mesa builds, so keep their
    implementations intact and put the whole private helper cluster under the
    feature guard instead of deleting it or marking the warnings away.
    """
    path = tree / "src/amd/common/ac_rgp.c"
    text = path.read_text(encoding="utf-8", errors="strict")

    text = replace_once(
        text,
        '#include <string.h>\n\n#define SQTT_FILE_MAGIC_NUMBER',
        '#include <string.h>\n\n#if defined(USE_LIBELF)\n#define SQTT_FILE_MAGIC_NUMBER',
        "RGP serializer feature guard start",
    )
    text = replace_once(
        text,
        '#if defined(USE_LIBELF)\nstatic void\nac_sqtt_dump_data(',
        'static void\nac_sqtt_dump_data(',
        "RGP serializer existing inner guard",
    )
    text = replace_once(
        text,
        '\n#endif\n\nstatic bool\nac_use_derived_spm_trace(',
        '\nstatic bool\nac_use_derived_spm_trace(',
        "RGP derived SPM serializer guard",
    )
    text = replace_once(
        text,
        '\n}\n\nint\nac_dump_rgp_capture(',
        '\n}\n#endif\n\nint\nac_dump_rgp_capture(',
        "RGP serializer feature guard end",
    )
    path.write_text(text, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-psbc-warning-sources.py <opengnm-psbc source tree>", file=sys.stderr)
        return 2
    tree = Path(sys.argv[1]).resolve()
    if not (tree / "src/compiler/nir/nir.h").is_file():
        raise SystemExit(f"not an opengnm-psbc source tree: {tree}")
    pass_calls = patch_nir_pass(tree)
    patch_spirv_fail(tree)
    patch_nir_print(tree)
    math_calls = patch_math_generic_calls(tree)
    patch_optional_generated_helper(tree)
    patch_miscellaneous(tree)
    patch_sample_mask_helper_use(tree)
    patch_radv_optional_and_shared_helpers(tree)
    patch_optional_rgp_serialization(tree)
    print(
        "PSBC source warnings: fixed ignored-progress passes "
        f"({pass_calls}), math generic calls ({math_calls}), SPIR-V fail paths, "
        "and concrete type/buffer diagnostics"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
