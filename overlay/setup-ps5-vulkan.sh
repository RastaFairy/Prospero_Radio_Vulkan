#!/usr/bin/env bash
set -euo pipefail

OVERLAY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Stage a title-linked PS5_Vulkan driver/runtime into one ProsperoRadio worktree.
# The script intentionally does not package a fake libvulkan.so: the PS5_Vulkan
# project documents that repository-built graphics modules cannot be dlopen()'d
# by a title on console, so the driver archives are linked into eboot.bin.
WORKTREE="$(cd "$1" && pwd)"
CACHE_ROOT="${2:-$WORKTREE/.ps5-vulkan-cache}"
mkdir -p "$CACHE_ROOT"
CACHE_ROOT="$(cd "$CACHE_ROOT" && pwd)"
PS5_VULKAN_REF="${PS5_VULKAN_REF:-085aac6a9e42c0d6337660e7148eb8990604052a}"
PS5_VULKAN_URL="${PS5_VULKAN_URL:-https://github.com/mihawk-99/PS5_Vulkan.git}"
VROOT="$CACHE_ROOT/PS5_Vulkan"
OUT="$WORKTREE/.local/vulkan"

mkdir -p "$CACHE_ROOT" "$OUT/lib" "$OUT/imports" "$OUT/include" "$OUT/meta"

if [[ ! -d "$VROOT/.git" ]]; then
    echo "==> Cloning PS5_Vulkan ($PS5_VULKAN_REF)"
    git clone "$PS5_VULKAN_URL" "$VROOT"
fi

cd "$VROOT"
git fetch --depth 1 origin "$PS5_VULKAN_REF"
git checkout --detach "$PS5_VULKAN_REF"
git reset --hard "$PS5_VULKAN_REF"


patch_ps5_vulkan_psbc_bootstrap() {
    local script="$VROOT/tools/build-psbc-ps5.sh"
    [[ -f "$script" ]] || { echo "Missing PS5_Vulkan PSBC bootstrap: $script" >&2; exit 2; }

    python3 - "$script" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

marker = "# Compute metadata: the pinned 0.2.0-era compiler kept ACO's rsrc1/rsrc2/rsrc3,"
if "u_format_table.py src/util/format/u_format.yaml --enums > src/util/format/u_format_gen.h" in text:
    raise SystemExit(0)

insert = r"""
# The vendored Mesa tree used by ps5-opengl 0.3.0 requires these generated
# files, but the standalone PS5_Vulkan make bootstrap does not generate them.
(
    cd "$tree"
    python3 src/util/format/u_format_table.py src/util/format/u_format.yaml --enums > src/util/format/u_format_gen.h
    python3 src/util/format/u_format_table.py src/util/format/u_format.yaml --header > src/util/format/u_format_pack.h
    python3 src/util/format/u_format_table.py src/util/format/u_format.yaml > src/util/format/u_format_table.c
    python3 src/util/format_srgb.py > src/util/format_srgb.c
    python3 src/compiler/builtin_types_h.py src/compiler/builtin_types.h
    python3 src/compiler/builtin_types_c.py src/compiler/builtin_types.c
    # NIR intrinsic metadata is required by the Vulkan runtime and must be
    # materialized before the PS5 archive is assembled. The upstream Makefile
    # lists these files as generated, but the standalone bootstrap must not
    # rely on a pre-generated checkout.
    python3 src/compiler/nir/nir_intrinsics_h.py --out src/compiler/nir/nir_intrinsics.h
    python3 src/compiler/nir/nir_intrinsics_c.py --out src/compiler/nir/nir_intrinsics.c
    python3 src/compiler/nir/nir_intrinsics_indices_h.py --out src/compiler/nir/nir_intrinsics_indices.h
    python3 src/util/process_shader_stats.py src/util/shader_stats.rnc src/util/shader_stats.xml > src/util/shader_stats.h
    python3 src/vulkan/util/vk_struct_type_cast_gen.py \
        --xml src/vulkan/registry/vk.xml \
        --out src/vulkan/util/vk_struct_type_cast.h \
        --beta false
    python3 src/amd/packets/parse_cp_pm4_table_data_json.py \
        src/amd/packets/cp_pm4_table_data_gfx11.json \
        src/amd/packets/pm4_it_opcodes_gfx11.h \
        src/amd/packets/cp_pm4_table_data_gfx12.json \
        src/amd/packets/pm4_it_opcodes_gfx12.h \
        gfx11 packets_h > src/amd/common/amd_cp_packets_gfx11.h
    python3 src/amd/packets/parse_cp_pm4_table_data_json.py \
        src/amd/packets/cp_pm4_table_data_gfx11.json \
        src/amd/packets/pm4_it_opcodes_gfx11.h \
        src/amd/packets/cp_pm4_table_data_gfx12.json \
        src/amd/packets/pm4_it_opcodes_gfx12.h \
        gfx12 packets_h > src/amd/common/amd_cp_packets_gfx12.h
    python3 src/amd/common/gfx10_format_table.py \
        src/util/format/u_format.yaml \
        src/amd/registers/gfx10-rsrc.json \
        src/amd/registers/gfx11-rsrc.json > src/amd/common/gfx10_format_table.c

    # The opengnm-psbc Makefile discovers Mesa C sources with $(wildcard), and
    # some generated C files may already exist when Make parses that list.
    # Add every required generated source while sorting the expanded list so a
    # source found by both the wildcard and this explicit list is compiled once.
    python3 - "$tree/Makefile" <<'PY_GENERATED_C_SOURCES'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

generated_sources = {
    "ACO_SRCS": [
        "src/amd/compiler/aco_opcodes.cpp",
    ],
    "NIR_SRCS": [
        "src/compiler/nir/nir_constant_expressions.c",
        "src/compiler/nir/nir_intrinsics.c",
        "src/compiler/nir/nir_opcodes.c",
        "src/compiler/nir/nir_opt_algebraic.c",
    ],
    "SPIRV_SRCS": [
        "src/compiler/spirv/spirv_info.c",
        "src/compiler/spirv/vtn_gather_types.c",
    ],
}

for variable, sources in generated_sources.items():
    marker = f"{variable} = "
    start = text.find(marker)
    if start < 0:
        raise SystemExit(f"{variable} definition not found")
    end = text.find("\n\n", start)
    if end < 0:
        raise SystemExit(f"{variable} block terminator not found")
    assignment = f"{variable} := $(sort $({variable}) {' '.join(sources)})"
    if assignment not in text:
        text = text[:end + 2] + assignment + "\n\n" + text[end + 2:]


path.write_text(text, encoding="utf-8")
PY_GENERATED_C_SOURCES

    # Force the PSBC archive to be rebuilt from the expanded source list while
    # preserving the rest of the compiler object's incremental cache.
    rm -f \
        "$tree/libpsbc.ps5.a" \
        "$tree/src/amd/compiler/aco_opcodes.ps5.cpp.o" \
        "$tree/src/compiler/nir/nir_constant_expressions.o" \
        "$tree/src/compiler/nir/nir_intrinsics.o" \
        "$tree/src/compiler/nir/nir_opcodes.o" \
        "$tree/src/compiler/nir/nir_opt_algebraic.o" \
        "$tree/src/compiler/spirv/spirv_info.o" \
        "$tree/src/compiler/spirv/vtn_gather_types.o"
)
"""

if marker not in text:
    raise SystemExit("PS5_Vulkan bootstrap marker not found; refusing a blind patch")

path.write_text(text.replace(marker, insert + "\n" + marker, 1), encoding="utf-8")
PY
    echo "==> PS5_Vulkan: patched PSBC generated-source bootstrap"
}

patch_ps5_vulkan_warning_policy() {
    local helper="$OVERLAY_ROOT/tools/patch-ps5-vulkan-warning-policy.py"
    [[ -f "$helper" ]] || { echo "Missing PS5_Vulkan warning-policy helper: $helper" >&2; exit 2; }
    python3 "$helper" "$VROOT"
}

patch_ps5_vulkan_warning_policy
patch_ps5_vulkan_psbc_bootstrap

# Build stages documented by PS5_Vulkan. Keep them explicit so a missing
# dependency stops here instead of producing an apparently-linkable dummy.
echo "==> PS5_Vulkan: dependency/bootstrap stage"
make deps

# The current PS5_Vulkan tree builds its compiler against the ps5-opengl 0.3.0
# SDK layout. The SDK is intentionally not vendored by PS5_Vulkan, so adapt it
# before any tool that resolves the SDK through tools/sdk-root.sh.
OPENGL_URL="${PS5_OPENGL_URL:-https://github.com/blackbearreloaded/ps5-opengl.git}"
OPENGL_REF="${PS5_OPENGL_REF:-v0.3.0}"
OPENGL_CACHE="$CACHE_ROOT/ps5-opengl-sdk-0.3.0"

if [[ -n "${PS5_OPENGL_SDK:-}" ]]; then
    OPENGL_INPUT="$PS5_OPENGL_SDK"
elif [[ -n "${PS5_OPENGL_RELEASE:-}" ]]; then
    OPENGL_INPUT="$PS5_OPENGL_RELEASE"
elif [[ -d "$OPENGL_CACHE/.git" ]]; then
    echo "==> PS5 OpenGL: updating cached checkout ($OPENGL_REF)"
    git -C "$OPENGL_CACHE" fetch --depth 1 origin "refs/tags/$OPENGL_REF:refs/tags/$OPENGL_REF"
    git -C "$OPENGL_CACHE" checkout --detach "$OPENGL_REF"
    OPENGL_INPUT="$OPENGL_CACHE"
elif [[ -d "$OPENGL_CACHE" ]]; then
    OPENGL_INPUT="$OPENGL_CACHE"
elif [[ -d "$HOME/ps5-opengl-sdk-0.3.0/.git" ]]; then
    OPENGL_INPUT="$HOME/ps5-opengl-sdk-0.3.0"
elif [[ -d "$VROOT/../ps5-opengl-sdk-0.3.0" ]]; then
    OPENGL_INPUT="$VROOT/../ps5-opengl-sdk-0.3.0"
elif git clone --depth 1 --branch "$OPENGL_REF" "$OPENGL_URL" "$OPENGL_CACHE"; then
    echo "==> PS5 OpenGL: cloned $OPENGL_REF"
    OPENGL_INPUT="$OPENGL_CACHE"
else
    echo "Missing PS5 OpenGL SDK 0.3.0 and automatic checkout failed." >&2
    echo "Set PS5_OPENGL_SDK to a checkout/tree, or PS5_OPENGL_RELEASE to the 0.3.0 release bundle." >&2
    echo "Automatic source: $OPENGL_URL ($OPENGL_REF)" >&2
    exit 2
fi

[[ -f "$OPENGL_INPUT/dependencies.json" ]] || { echo "PS5 OpenGL input is missing dependencies.json: $OPENGL_INPUT" >&2; exit 2; }
[[ -f "$OPENGL_INPUT/toolchain/opengnm-psbc-host.mak" ]] || { echo "PS5 OpenGL input is missing toolchain/opengnm-psbc-host.mak: $OPENGL_INPUT" >&2; exit 2; }

echo "==> PS5 OpenGL: fetching pinned third_party sources"
(cd "$OPENGL_INPUT" && python3 tools/fetch-sources.py)

echo "==> PS5_Vulkan: building pinned PS5 shader compiler work copy"
# Upstream order is intentional: adapt-opengl-sdk.sh consumes the compiler
# work copy produced by build-psbc-ps5.sh. Pass the freshly resolved 0.3.0
# checkout explicitly so sdk-root.sh does not fall back to an older 0.2.0 tree.
PS5_OPENGL_SDK="$OPENGL_INPUT" bash tools/build-psbc-ps5.sh

PSBC_ARCHIVE="$VROOT/.deps/native/psbc/lib/libpsbc.ps5.a"
[[ -f "$PSBC_ARCHIVE" ]] || { echo "PSBC archive was not installed: $PSBC_ARCHIVE" >&2; exit 2; }
NIR_REQUIRED=(
    nir_intrinsic_infos
    nir_op_infos
    nir_eval_const_opcode
    nir_type_conversion_op
    nir_opt_algebraic
    nir_opt_algebraic_late
    nir_opt_reassociate_for_fma
)
PSBC_DEFINED="$($VROOT/.deps/native/ps5-payload-sdk/bin/prospero-nm --defined-only "$PSBC_ARCHIVE" 2>/dev/null | awk '{print $NF}' | sort -u)"
for symbol in "${NIR_REQUIRED[@]}"; do
    if ! grep -Fxq "$symbol" <<<"$PSBC_DEFINED"; then
        echo "PSBC archive is missing generated NIR symbol: $symbol" >&2
        echo "The corresponding generated C source was not archived by the PSBC Makefile." >&2
        exit 2
    fi
done

if ! grep -Fxq '_ZN3aco10instr_infoE' <<<"$PSBC_DEFINED"; then
    echo "PSBC archive is missing ACO opcode metadata symbol aco::instr_info" >&2
    echo "The generated aco_opcodes.cpp source was not archived by the PSBC Makefile." >&2
    exit 2
fi

echo "==> PS5_Vulkan: adapting PS5 OpenGL SDK ($OPENGL_REF)"
PS5_OPENGL_SDK="$OPENGL_INPUT" bash tools/adapt-opengl-sdk.sh "$OPENGL_INPUT"

echo "==> PS5_Vulkan: Mesa/PS5BC sources"
bash tools/fetch-mesa.sh

echo "==> PS5_Vulkan: Vulkan runtime"
bash tools/build-vulkan-runtime.sh

# PS5_Vulkan's build-driver.sh signs/extracts its PS5 shared-object test
# modules with the native converter from the application boilerplate. The
# driver repository expects that converter at its own build/host path, while
# its source actually belongs to ProsperoRadio. Build a copy there so the
# dependency can remain pinned and otherwise unmodified.
prepare_ps5_native_tool() {
    local tool="$VROOT/build/host/ps5-native-tool"
    local native="$WORKTREE/tooling/native"
    local zlib_root="$VROOT/.deps/native/zlib/root"
    local zlib_archive
    local cxx

    [[ -f "$native/native_app_builder.cpp" ]] || { echo "Missing native_app_builder.cpp in worktree" >&2; exit 2; }
    [[ -f "$native/self_container.cpp" ]] || { echo "Missing self_container.cpp in worktree" >&2; exit 2; }
    [[ -f "$native/elf_object.cpp" ]] || { echo "Missing elf_object.cpp in worktree" >&2; exit 2; }
    [[ -f "$native/sce_module_writer.cpp" ]] || { echo "Missing sce_module_writer.cpp in worktree" >&2; exit 2; }
    zlib_archive=$(find "$zlib_root" -type f -name libz.a -print -quit)
    [[ -n "$zlib_archive" && -f "$zlib_archive" ]] || { echo "Missing host zlib archive for ps5-native-tool" >&2; exit 2; }

    cxx="${CXX:-}"
    if [[ -z "$cxx" ]]; then
        cxx=$(command -v clang++-18 || command -v clang++ || true)
    fi
    [[ -n "$cxx" ]] || { echo "Clang++ is required to build ps5-native-tool" >&2; exit 2; }

    mkdir -p "$VROOT/build/host"
    "$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
        -I "$zlib_root/usr/include" \
        "$native/native_app_builder.cpp" \
        "$native/self_container.cpp" \
        "$native/elf_object.cpp" \
        "$native/sce_module_writer.cpp" \
        "$zlib_archive" -o "$tool"
    chmod +x "$tool"
    echo "==> PS5_Vulkan: staged ProsperoRadio ps5-native-tool"
}

prepare_ps5_native_tool

echo "==> PS5_Vulkan: PS5 driver"
bash tools/build-driver.sh

DRIVER_PSBC_ARCHIVE="$VROOT/build/driver/ps5/libpsbc_driver.ps5.a"
[[ -f "$DRIVER_PSBC_ARCHIVE" ]] || { echo "PS5 driver compiler archive missing: $DRIVER_PSBC_ARCHIVE" >&2; exit 2; }
if ! "$VROOT/.deps/native/ps5-payload-sdk/bin/prospero-nm" --defined-only "$DRIVER_PSBC_ARCHIVE" 2>/dev/null |
    awk 'BEGIN { found = 0 } $NF == "nir_intrinsic_infos" { found = 1 } END { exit !found }'; then
    echo "PS5 driver compiler archive is missing nir_intrinsic_infos" >&2
    echo "The driver archive cannot satisfy Mesa NIR's generated intrinsic metadata for the title link." >&2
    exit 2
fi

echo "==> ProsperoRadio: Mesa utility objects required by the static driver"
PS5_VULKAN_DIR="$VROOT" PS5_PAYLOAD_SDK="$VROOT/.deps/native/ps5-payload-sdk" \
    bash "$WORKTREE/tools/build-mesa-util.sh" > "$OUT/meta/mesa-util-objects.txt"

DRIVER_LIB="$VROOT/build/driver/ps5/libps5vk.ps5.a"
RUNTIME_LIB="$VROOT/.deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a"
[[ -f "$DRIVER_LIB" ]] || { echo "Missing $DRIVER_LIB" >&2; exit 1; }
[[ -f "$RUNTIME_LIB" ]] || { echo "Missing $RUNTIME_LIB" >&2; exit 1; }

rm -f "$OUT/lib"/*.a "$OUT/imports"/*.so

# Collect the PS5 static archives required by the title. The driver build has
# already embedded a renamed copy of libpsbc.ps5.a as libpsbc_driver.ps5.a;
# staging the original libpsbc.ps5.a as well would define every PSBC entry point
# twice and make the final title link fail with duplicate symbols. Keep the
# support archive because it contains PS5-specific package/S3TC/process helpers.
mapfile -t ARCHIVES < <(
    find "$VROOT/build/driver/ps5" "$VROOT/.deps/native/vulkan-runtime/lib" "$VROOT/.deps/native/psbc/lib" \
        -type f -name '*.ps5.a' ! -name '*.pic.a' ! -name 'libpsbc.ps5.a' -print 2>/dev/null | sort -u
)
((${#ARCHIVES[@]} > 0)) || { echo "No PS5 Vulkan static archives were produced" >&2; exit 1; }
for archive in "${ARCHIVES[@]}"; do
    cp -f "$archive" "$OUT/lib/"
done

# The PS5 Vulkan build emits the Sony import stubs needed by the AGC-facing
# driver. Keep only SCE import libraries; do not pass libvulkan.so.1 to the app.
mapfile -t STUBS < <(find "$VROOT/build/driver/ps5" -type f -name 'libSce*.so' -print 2>/dev/null | sort -u)
((${#STUBS[@]} > 0)) || { echo "No PS5 SCE import stubs were produced by PS5_Vulkan" >&2; exit 1; }
for stub in "${STUBS[@]}"; do
    cp -f "$stub" "$OUT/imports/"
done

HEADER="$(find "$VROOT" -type f -path '*/include/vulkan/vulkan_core.h' -print -quit 2>/dev/null || true)"
if [[ -z "$HEADER" ]]; then
    HEADER="$(find "$VROOT" -type f -path '*/include/vulkan.h' -print -quit 2>/dev/null || true)"
fi
[[ -n "$HEADER" ]] || { echo "Vulkan headers were not found inside PS5_Vulkan" >&2; exit 1; }

# Vulkan-Headers keeps generated video-standard headers beside the main
# include/vulkan directory:
#   include/vulkan/vulkan_core.h
#   include/vk_video/vulkan_video_codec_*.h
# Copy both directories. Copying only include/vulkan makes vulkan_core.h
# compile until it reaches VK_KHR_video_* and then fails on vk_video/*.h.
HEADER_DIR="$(dirname "$HEADER")"
if [[ "$(basename "$HEADER_DIR")" == "vulkan" ]]; then
    INCLUDE_ROOT="$(dirname "$HEADER_DIR")"
else
    INCLUDE_ROOT="$HEADER_DIR"
fi
[[ -d "$INCLUDE_ROOT/vulkan" ]] || {
    echo "Vulkan include root is missing $INCLUDE_ROOT/vulkan" >&2
    exit 1
}
rm -rf "$OUT/include"
mkdir -p "$OUT/include"
cp -a "$INCLUDE_ROOT"/vulkan "$OUT/include/"
if [[ -d "$INCLUDE_ROOT/vk_video" ]]; then
    cp -a "$INCLUDE_ROOT/vk_video" "$OUT/include/"
else
    echo "warning: Vulkan-Headers has no vk_video directory at $INCLUDE_ROOT/vk_video" >&2
    echo "warning: headers including VK_KHR_video_* will not compile" >&2
fi

GLSLANG="$(command -v glslangValidator || true)"
if [[ -z "$GLSLANG" ]]; then
    GLSLANG="$(find "$VROOT/.deps" -type f -name glslangValidator -print -quit 2>/dev/null || true)"
fi
[[ -n "$GLSLANG" ]] || { echo "glslangValidator is required to compile the ProsperoRadio UI shaders" >&2; exit 1; }

mkdir -p "$WORKTREE/assets/ui/vulkan"
"$GLSLANG" -V --target-env vulkan1.0 "$OVERLAY_ROOT/assets/ui/vulkan/ui.vert" -o "$WORKTREE/assets/ui/vulkan/ui.vert.spv"
"$GLSLANG" -V --target-env vulkan1.0 "$OVERLAY_ROOT/assets/ui/vulkan/ui.frag" -o "$WORKTREE/assets/ui/vulkan/ui.frag.spv"

printf '%s\n' "$PS5_VULKAN_URL" > "$OUT/meta/repository.txt"
printf '%s\n' "$(git rev-parse HEAD)" > "$OUT/meta/commit.txt"
printf '%s\n' "$DRIVER_LIB" > "$OUT/meta/driver-source.txt"
printf '%s\n' "$RUNTIME_LIB" > "$OUT/meta/runtime-source.txt"
printf '%s\n' "$GLSLANG" > "$OUT/meta/glslang.txt"

# Space-separated because the upstream build scripts consume these settings as
# shell word lists. The app's normal archives remain in the Makefile as before.
printf '%s\n' "${ARCHIVES[@]}" | sed "s#^$VROOT/#$OUT/upstream-cache/#" > "$OUT/meta/archive-sources.txt"
printf '%s\n' "${STUBS[@]}" | sed "s#^$VROOT/#$OUT/upstream-cache/#" > "$OUT/meta/import-sources.txt"

printf '==> Staged PS5_Vulkan commit: %s\n' "$(git rev-parse HEAD)"
printf '==> Static archives: %s\n' "${#ARCHIVES[@]}"
printf '==> SCE import stubs: %s\n' "${#STUBS[@]}"
printf '==> Vulkan headers: %s\n' "$OUT/include/vulkan"
printf '==> UI SPIR-V: %s\n' "$WORKTREE/assets/ui/vulkan"
