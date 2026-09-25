#!/usr/bin/env bash
# ProsperoRadio - Mesa utility objects required by the linked PS5 Vulkan driver.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the three Mesa utility sources intentionally omitted from the
# PS5_Vulkan static object list. A shared driver can leave these unresolved;
# the final eboot link cannot, so the title supplies them as plain objects.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${1:-${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}}
vulkan_dir=${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}
third_party="$vulkan_dir/.deps/work/psbc-ps5/third_party"
mesa="$third_party/opengnm-psbc"
out="$root/build/obj/mesa-util"

[[ -x "$sdk/bin/prospero-clang" ]] || {
    echo "missing PS5 compiler: $sdk/bin/prospero-clang" >&2
    exit 2
}
for directory in "$third_party/opengnm/include" "$third_party/Vulkan-Headers/include"; do
    [[ -d "$directory" ]] || {
        echo "missing PS5 Vulkan include tree: $directory" >&2
        exit 2
    }
done

shared_flags=(
    -Iinclude/
    -Ilibpsbc/
    -I"$third_party/opengnm/include"
    -I"$third_party/Vulkan-Headers/include"
    -Isrc/
    -Isrc/amd
    -Isrc/amd/common
    -Isrc/amd/common/nir
    -Isrc/amd/compiler
    -Isrc/amd/vulkan
    -Isrc/amd/vulkan/nir
    -Isrc/vulkan/runtime
    -Isrc/vulkan/runtime/bvh
    -Isrc/vulkan/util
    -Isrc/compiler
    -Isrc/compiler/nir
    -Isrc/compiler/spirv
    -Isrc/gallium/include
    -Isrc/mesa
    -Isrc/mesa/main
    -Isrc/util
    -Icmd/psbc
    -Iinclude/mesa
    -D_GNU_SOURCE
    -D_XOPEN_SOURCE=700
    -DUTIL_ARCH_LITTLE_ENDIAN=1
    -DUTIL_ARCH_BIG_ENDIAN=0
    -DHAVE_STRUCT_TIMESPEC=1
    -DHAVE_PTHREAD=1
    -DHAVE_PTHREAD_NP_H=1
    -DHAVE_SYSCONF=1
    -DHAVE_FUNC_ATTRIBUTE_PACKED=1
    -Dalloca=__builtin_alloca
    -include
    strings.h
    -DBLAKE3_NO_SSE2
    -DBLAKE3_NO_SSE41
    -DBLAKE3_NO_AVX2
    -DBLAKE3_NO_AVX512
)

mkdir -p "$out"
sources=(src/util/u_thread.c src/util/anon_file.c src/util/os_file.c)
for source in "${sources[@]}"; do
    [[ -f "$mesa/$source" ]] || {
        echo "missing Mesa source: $mesa/$source" >&2
        exit 2
    }
    object="$out/${source##*/}"
    object="${object%.c}.o"
    flags=("${shared_flags[@]}")
    case "${source##*/}" in
        anon_file.c)
            filtered=()
            for flag in "${flags[@]}"; do
                [[ $flag == -D_XOPEN_SOURCE=700 ]] || filtered+=("$flag")
            done
            flags=("${filtered[@]}")
            ;;
        os_file.c)
            flags+=(-D__ORBIS__)
            ;;
    esac
    (
        cd "$mesa"
        PS5_PAYLOAD_SDK="$sdk" sh "$root/tooling/prospero-clang18" \
            -std=gnu11 -O2 -g -Wall -fPIC -DOPENGNM_PSBC_ORBIS=1 \
            -Dstatic_assert=_Static_assert \
            -Wno-unused-function -Wno-unused-variable \
            -Wno-unreachable-code-generic-assoc \
            "${flags[@]}" -c "$source" -o "$object"
    )
    [[ -f "$object" ]] || {
        echo "failed to build Mesa utility object: $object" >&2
        exit 1
    }
done

printf '%s\n' \
    "$out/u_thread.o" \
    "$out/anon_file.o" \
    "$out/os_file.o"
