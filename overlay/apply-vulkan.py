#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

from console_ux_patch import patch_console_ux

UPSTREAM_SHA = "33898dd35375c1ae8370da137cfb6941d91c7684"
UPSTREAM_SHA_CURRENT = "c0643ed66212a57819e04577d14528ba8238f197"  # v01.000.023 (allow this too)
VERSION = "02.000.055"

# Fork of the boilerplate allocation runtime (overlay/src/app_cpp_runtime.cpp).
# 01.000.016 redirected title stderr into /download0/prospero-radio.log and gave
# allocation failures a diagnosable abort. 01.000.017 adds a Direct-Memory pool:
# the libc heap on the console cannot grow past a few MB (measured on hardware:
# calloc(1.2 MB) for the font glyph table returns NULL at startup), so the link
# wraps malloc/calloc/realloc/free/posix_memalign and allocations >= 256 KB are
# served by a 512 MB Direct-Memory pool with a reuse cache; small allocations
# keep using the libc heap.


def patch_cpp_runtime(worktree: Path) -> None:
    source = Path(__file__).resolve().parent / "src" / "app_cpp_runtime.cpp"
    target = worktree / "tooling" / "native" / "app_cpp_runtime.cpp"
    if not target.exists():
        raise RuntimeError(f"missing upstream runtime source: {target}")
    shutil.copy2(source, target)
    # Keep the runtime log banner in lockstep with the package version so the
    # telemetry identifies the installed binary.
    text = target.read_text(encoding="utf-8")
    if "__RUNTIME_VERSION__" not in text:
        raise RuntimeError("runtime banner token missing from app_cpp_runtime.cpp")
    target.write_text(text.replace("__RUNTIME_VERSION__", VERSION), encoding="utf-8")


def run(*args: str, cwd: Path) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Expected exactly one occurrence in {path}, got {count}: {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def replace_function(path: Path, signature: str, replacement: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(signature) != 1:
        raise RuntimeError(
            f"Expected exactly one function signature in {path}, got {text.count(signature)}: {signature}"
        )
    start = text.index(signature)
    open_brace = text.find("{", start)
    close_brace = find_matching_brace(text, open_brace)
    path.write_text(text[:start] + replacement + text[close_brace + 1 :], encoding="utf-8")


def find_matching_brace(text: str, open_index: int) -> int:
    if text[open_index] != "{":
        raise ValueError("open_index is not a brace")
    depth = 0
    in_string = None
    escape = False
    line_comment = False
    block_comment = False
    i = open_index
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if ch == "\n":
                line_comment = False
            i += 1
            continue
        if block_comment:
            if ch == "*" and nxt == "/":
                block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == in_string:
                in_string = None
            i += 1
            continue
        if ch == "/" and nxt == "/":
            line_comment = True
            i += 2
            continue
        if ch == "/" and nxt == "*":
            block_comment = True
            i += 2
            continue
        if ch in ('"', "'"):
            in_string = ch
            i += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("Unmatched brace")


def replace_class(text: str, class_name: str, replacement: str) -> str:
    marker = f"class {class_name} final"
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"Could not locate {marker}")
    brace = text.find("{", start)
    end = find_matching_brace(text, brace)
    end += 1
    return text[:start] + replacement + text[end:]


def replace_runapp_body(text: str) -> str:
    match = re.search(r"(?m)^(\s*(?:bool|int)\s+RunApp\s*\([^;]*\)\s*\{)", text)
    if not match:
        raise RuntimeError("Could not locate RunApp() in upstream main.cpp")
    open_brace = text.find("{", match.start())
    close_brace = find_matching_brace(text, open_brace)
    replacement = r'''bool RunApp()
{
    if (SDL_SetMemoryFunctions(AllocateTracked, CallocTracked, ReallocTracked, FreeTracked) != 0)
        return false;

    SDL_SetMainReady();
    if (SDL_Init(0) != 0)
        return false;

    AppSystemInterface system_interface;
    AppFileInterface file_interface;
    ProsperoVulkanRenderInterfaceAdapter render_interface;
    if (!render_interface.IsInitialized())
    {
        SDL_Quit();
        return false;
    }
    BitmapFontEngine font_engine;
    Rml::RenderInterface *adapted_render_interface = render_interface.GetAdaptedInterface();
    Rml::SetSystemInterface(&system_interface);
    Rml::SetFileInterface(&file_interface);
    Rml::SetRenderInterface(adapted_render_interface);
    Rml::SetFontEngineInterface(&font_engine);

    bool running = Rml::Initialise();
    if (running)
        running = LoadFonts();
    Rml::Context *context =
        running ? Rml::CreateContext("radio-browser", {1920, 1080}, adapted_render_interface)
                : nullptr;
    Rml::ElementDocument *document =
        context ? context->LoadDocument("assets/ui/main.rml") : nullptr;
    RadioApp app;
    bool input_ready = false;
    bool ime_ready = false;
    if (document)
    {
        document->Show();
        input_ready = radio_input_init();
        ime_ready = input_ready && radio_ime_init();
        running = input_ready && ime_ready && app.Initialize(document);
        if (running)
            sceSystemServiceHideSplashScreen();
    }
    else
    {
        running = false;
    }

    while (running)
    {
        radio_input_poll();
        radio_input_event_t input{};
        while (radio_input_next(&input))
            app.HandleInput(input);
        radio_ime_poll();
        app.Poll();
        if (app.WantsQuit())
            running = false;
        context->Update();
        render_interface.BeginFrame();
        context->Render();
        if (!render_interface.EndFrame())
            running = false;
        ProsperoRuntimeLogMaintenance();
        sceKernelUsleep(16667);
    }

    app.Shutdown();
    if (ime_ready)
        radio_ime_shutdown();
    if (input_ready)
        radio_input_shutdown();
    if (document)
        document->Close();
    if (context)
        Rml::RemoveContext("radio-browser");
    Rml::Shutdown();
    SDL_Quit();
    return false;
}'''
    return text[:match.start()] + replacement + text[close_brace + 1:]


def remove_present_color(text: str) -> str:
    match = re.search(r"(?m)^\s*void\s+PresentColor\s*\([^)]*\)\s*\{", text)
    if not match:
        raise RuntimeError("Could not locate the obsolete SDL PresentColor helper")
    open_brace = text.find("{", match.start())
    close_brace = find_matching_brace(text, open_brace)
    return text[:match.start()] + text[close_brace + 1:]




def remove_title_compat_duplicates(worktree: Path) -> None:
    main = worktree / "src" / "main.cpp"
    text = main.read_text(encoding="utf-8")
    block = 'extern "C" void __assert(const char *, const char *, int, const char *)\n{\n    std::abort();\n}\n\n'
    if block not in text:
        raise RuntimeError("Expected title-local __assert compatibility block in main.cpp")
    text = text.replace(block, "", 1)
    main.write_text(text, encoding="utf-8")

    sqlite = worktree / "src" / "sqlite_compat.cpp"
    text = sqlite.read_text(encoding="utf-8")
    block = 'extern "C" struct tm *localtime_r(const time_t *timer, struct tm *result)\n{\n    (void)timer;\n    (void)result;\n    return nullptr;\n}\n\n'
    if block not in text:
        raise RuntimeError("Expected title-local localtime_r compatibility block in sqlite_compat.cpp")
    text = text.replace(block, "", 1)
    sqlite.write_text(text, encoding="utf-8")


def patch_link_duplicate_audit(worktree: Path) -> None:
    build_script = worktree / "tools" / "build.sh"
    text = build_script.read_text(encoding="utf-8")
    linker_line = re.compile(
        r'(?m)^"\$sdk_root/bin/prospero-lld" -T "\$native/ps5-pie\.ld" --eh-frame-hdr.*$'
    )
    matches = list(linker_line.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(
            f"Could not locate unique final ProsperoRadio linker invocation (found {len(matches)})"
        )
    audit = "\n".join([
        'python3 "$root/tools/check-vulkan-link-duplicates.py" \\',
        '    --nm "$sdk_root/bin/prospero-nm" \\',
        '    --objects "${objects[@]}" "${extra_objects[@]}" \\',
        '    --archives "${vulkan_archives[@]}"',
    ]) + "\n\n"
    insert_at = matches[0].start()
    text = text[:insert_at] + audit + text[insert_at:]
    linker_line_match = linker_line.search(text, insert_at + len(audit))
    if linker_line_match is None:
        raise RuntimeError("Could not re-locate final ProsperoRadio linker invocation after audit insertion")
    linker_end = re.search(
        r'(?m)^    --as-needed "\$sdk_root"/target/lib/\*\.so$\n',
        text[linker_line_match.end():],
    )
    if linker_end is None:
        raise RuntimeError("Could not locate the end of the final ProsperoRadio linker invocation")
    post_link = """
# Audit the ordinary ELF symbol table, not the dynamic table. `-D` only sees
# symbols participating in dynamic linking and can report internal/static
# compiler symbols as imports even when they are fully resolved in the final
# executable. Weak undefined Vulkan entry points are intentionally allowed.
vk_undefined=$(
    "$sdk_root/bin/prospero-nm" --undefined-only --format=posix "$build/llvm-pie.elf" 2>/dev/null |
        awk '$2 == "U" && $1 ~ /^vk[A-Z]/ { print $1 }' | sort -u
)
if [[ -n $vk_undefined ]]; then
    echo "Vulkan link audit: unresolved strong Vulkan symbols remain after final link:" >&2
    printf '  %s\n' $vk_undefined >&2
    exit 2
fi

mesa_internal_undefined=$(
    "$sdk_root/bin/prospero-nm" --undefined-only --format=posix "$build/llvm-pie.elf" 2>/dev/null |
        awk '$2 == "U" && $1 ~ /^nir_/ { print $1 }' | sort -u
)
if [[ -n $mesa_internal_undefined ]]; then
    echo "Mesa link audit: unresolved NIR internal symbols remain after final link:" >&2
    printf '  %s\n' $mesa_internal_undefined >&2
    exit 2
fi

"""
    post_at = linker_line_match.end() + linker_end.end()
    text = text[:post_at] + post_link + text[post_at:]
    build_script.write_text(text, encoding="utf-8")

def stage_overlay_tools(worktree: Path, overlay: Path) -> None:
    tools_dir = worktree / "tools"
    tools_dir.mkdir(parents=True, exist_ok=True)
    for relative in (
        "tools/build-mesa-util.sh",
        "tools/check-vulkan-link-duplicates.py",
    ):
        src = overlay / relative
        if not src.is_file():
            raise RuntimeError(f"Required overlay tooling is missing: {src}")
        shutil.copy2(src, tools_dir / src.name)

    staged_audit = tools_dir / "check-vulkan-link-duplicates.py"
    if not staged_audit.is_file():
        raise RuntimeError(f"Duplicate-symbol audit was not staged: {staged_audit}")


def patch_target_build_driver(worktree: Path) -> None:
    build_script = worktree / "tools" / "build.sh"
    text = build_script.read_text(encoding="utf-8")

    old_arrays = '''definitions=()
cxx_flags=()
includes=()
archives=()
import_stubs=()'''
    new_arrays = '''definitions=()
cxx_flags=()
includes=()
archives=()
vulkan_archives=()
extra_objects=()
import_stubs=()'''
    if text.count(old_arrays) != 1:
        raise RuntimeError("Could not locate upstream linker input arrays")
    text = text.replace(old_arrays, new_arrays, 1)

    old_reads = '''[[ -z ${APP_STATIC_ARCHIVES:-} ]] || read -r -a archives <<< "$APP_STATIC_ARCHIVES"
[[ -z ${APP_IMPORT_STUBS:-} ]] || read -r -a import_stubs <<< "$APP_IMPORT_STUBS"'''
    new_reads = '''[[ -z ${APP_STATIC_ARCHIVES:-} ]] || read -r -a archives <<< "$APP_STATIC_ARCHIVES"
[[ -z ${APP_VULKAN_ARCHIVES:-} ]] || read -r -a vulkan_archives <<< "$APP_VULKAN_ARCHIVES"
[[ -z ${APP_EXTRA_OBJECTS:-} ]] || read -r -a extra_objects <<< "$APP_EXTRA_OBJECTS"
[[ -z ${APP_IMPORT_STUBS:-} ]] || read -r -a import_stubs <<< "$APP_IMPORT_STUBS"'''
    if text.count(old_reads) != 1:
        raise RuntimeError("Could not locate upstream APP_* input parsing")
    text = text.replace(old_reads, new_reads, 1)

    link_marker = 'link_inputs=("$build/obj/app_crt.o" "$build/obj/app_cpp_runtime.o" "${objects[@]}")\n'
    if text.count(link_marker) != 1:
        raise RuntimeError("Could not locate upstream link input assembly")
    vulkan_link = r'''if (( ${#vulkan_archives[@]} > 0 )); then
    for archive in "${vulkan_archives[@]}"; do
        [[ $archive =~ ^[A-Za-z0-9_./+-]+(/[A-Za-z0-9_.+-]+)*\.a$ && -f $root/$archive ]] || {
            echo "invalid Vulkan archive: $archive" >&2; exit 2;
        }
    done
    # Mesa's generated NIR core is split across several archive members. The
    # title link intentionally permits unresolved SCE imports, so do not rely on
    # incidental reference discovery to extract these members from libpsbc_driver.
    # Force one anchor from each generated object that exports NIR core APIs.
    nir_link_anchors=(
        nir_intrinsic_infos
        nir_op_infos
        nir_eval_const_opcode
        nir_type_conversion_op
        nir_opt_algebraic
        nir_opt_algebraic_late
        nir_opt_reassociate_for_fma
    )
    for symbol in "${nir_link_anchors[@]}"; do
        link_inputs+=("--undefined=$symbol")
    done
    link_inputs+=(--whole-archive)
    link_inputs+=("${vulkan_archives[@]}")
    link_inputs+=(--no-whole-archive)

    for object in "${extra_objects[@]}"; do
        [[ $object =~ ^[A-Za-z0-9_./+-]+(/[A-Za-z0-9_.+-]+)*\.o$ && -f $root/$object ]] || {
            echo "invalid extra Vulkan object: $object" >&2; exit 2;
        }
    done
    link_inputs+=("${extra_objects[@]}")

    vulkan_cxx=(
        "$sdk_root/target/lib/libc++.a"
        "$sdk_root/target/lib/libc++abi.a"
        "$sdk_root/target/lib/libunwind.a"
    )
    for archive in "${vulkan_cxx[@]}"; do
        [[ -f $archive ]] || {
            echo "missing Vulkan C++ runtime archive: $archive" >&2; exit 2;
        }
    done
    link_inputs+=(--start-group "${vulkan_cxx[@]}")
    target_clang=${PS5_CLANG:-${cxx}}
    builtins=""
    if [[ -n $target_clang ]]; then
        resource_dir=$("$target_clang" --print-resource-dir 2>/dev/null || true)
        candidate="$resource_dir/lib/linux/libclang_rt.builtins-x86_64.a"
        [[ -f $candidate ]] && builtins="$candidate"
    fi
    [[ -z $builtins ]] || link_inputs+=("$builtins")
    link_inputs+=(--end-group)
fi
'''
    allocator_wrap = (
        "# Large allocations are served by the title Direct-Memory pool\n"
        "# (tooling/native/app_cpp_runtime.cpp); the default libc heap cannot\n"
        "# grow past a few MB on this console.\n"
        'link_inputs+=("--wrap=malloc" "--wrap=calloc" "--wrap=realloc" "--wrap=free" '
        '"--wrap=posix_memalign")\n\n'
    )

    text = text.replace(link_marker, link_marker + allocator_wrap + vulkan_link, 1)

    start_marker = 'builder_stub_args=()\nlink_imports=()\nfor stub in "${import_stubs[@]}"; do'
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError("Could not locate the upstream import-stub loop in tools/build.sh")
    end_marker = '# The public SDK lacks a CommonDialog import stub'
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError("Could not locate the end of the upstream import-stub loop")
    replacement = """builder_stub_args=()
link_imports=()
for stub in "${import_stubs[@]}"; do
    [[ $stub =~ ^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*\\.(so|sprx|a)$ && -f $root/$stub ]] || {
        echo "invalid import stub path: $stub" >&2; exit 2;
    }
    builder_stub_args+=(--stub "$root/$stub")
    case "${stub##*/}" in
        libSceOpusDec_stub.a)
            link_source="$native/prospero_radio_import_stub_opus.cpp"
            soname=libSceOpusDec.sprx
            link_name=libSceOpusDec.so
            ;;
        libSceOpusCeltDec_stub.a)
            link_source="$native/prospero_radio_import_stub_opus_celt.cpp"
            soname=libSceOpusCeltDec.sprx
            link_name=libSceOpusCeltDec.so
            ;;
        *.so|*.sprx)
            link_inputs+=("$root/$stub")
            continue
            ;;
        *)
            echo "unsupported ProsperoRadio import stub: $stub" >&2; exit 2
            ;;
    esac
    mkdir -p "$build/import-stubs"
    link_object="$build/import-stubs/${link_name%.so}.o"
    link_stub="$build/import-stubs/$link_name"
    PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" \
        -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -fPIC \
        -c "$link_source" -o "$link_object"
    "$sdk_root/bin/prospero-lld" --shared -soname "$soname" -o "$link_stub" "$link_object"
    link_inputs+=("$link_stub")
done
"""
    build_script.write_text(text[:start] + replacement + text[end:], encoding="utf-8")


def patch_native_weak_imports(worktree: Path) -> None:
    writer = worktree / "tooling" / "native" / "sce_module_writer.cpp"

    replace_once(
        writer,
        """    std::vector<Import> imports;
    std::vector<const Stub *> module_order;
""",
        """    std::vector<Import> imports;
    std::vector<const Stub *> module_order;
    std::vector<bool> imported_symbols(image.dynamic_symbols.size());
""",
    )
    replace_once(
        writer,
        """        require(provider != nullptr, "no public SDK stub exports required symbol " + symbol.name);
        imports.push_back({symbol.name, provider, 0, 0, static_cast<std::uint32_t>(i), {}});
""",
        """        // ELF unresolved weak symbols resolve to zero when no provider exists.
        // Keep them as weak dynamic symbols instead of inventing a PS5 SDK import.
        if (provider == nullptr && symbol.weak())
            continue;
        require(provider != nullptr, "no public SDK stub exports required symbol " + symbol.name);
        imports.push_back({symbol.name, provider, 0, 0, static_cast<std::uint32_t>(i), {}});
        imported_symbols[i] = true;
""",
    )
    replace_once(
        writer,
        """        hash_names[import.dynamic_symbol] = nid(import.plain) + "#" +
                                            import.provider->library_name + "#" +
                                            import.provider->module_name;
    }
    const Bytes dynamic_strings = strings.data();
""",
        """        hash_names[import.dynamic_symbol] = nid(import.plain) + "#" +
                                            import.provider->library_name + "#" +
                                            import.provider->module_name;
    }
    // Preserve unprovided weak symbols in dynsym so their relocations keep ELF's
    // zero-resolution behavior; they do not belong in the SDK import hash.
    for (std::size_t i = 1; i < image.dynamic_symbols.size(); ++i)
    {
        const elf::Symbol &source = image.dynamic_symbols[i];
        if (!source.undefined() || !source.weak() || imported_symbols[i])
            continue;
        const std::size_t at = i * 24;
        write_u32(dynamic_symbols, at, strings.add(source.name));
        dynamic_symbols[at + 4] = source.info;
        dynamic_symbols[at + 5] = source.other;
        write_u16(dynamic_symbols, at + 6, source.section);
        write_u64(dynamic_symbols, at + 8, source.value);
        write_u64(dynamic_symbols, at + 16, source.size);
    }
    const Bytes dynamic_strings = strings.data();
""",
    )


def patch_v027_radio_features(worktree: Path) -> None:
    """Touchpad zones, lightbar, 12-band EQ on the PCM path and the AUX HTTP
    ingest server. Everything follows the established extern "C" pattern: no
    SDK headers, runtime symbol resolution, inert-safe failure modes."""
    # ---- input: touchpad state + lightbar setter --------------------------
    input_header = worktree / "include" / "radio_input.hpp"
    replace_once(
        input_header,
        "    RADIO_INPUT_STATION_NEXT,\n    RADIO_INPUT_COUNT",
        "    RADIO_INPUT_STATION_NEXT,\n    RADIO_INPUT_PAD_CLICK,\n    RADIO_INPUT_COUNT",
    )
    replace_once(
        input_header,
        "struct radio_input_event_t {\n    radio_input_key_t key;\n    bool pressed;\n};",
        "struct radio_input_event_t {\n    radio_input_key_t key;\n    bool pressed;\n"
        "    bool touch_contact;\n    unsigned short touch_x;\n    unsigned short touch_y;\n};",
    )
    replace_once(
        input_header,
        "void radio_input_shutdown(void);",
        "void radio_input_shutdown(void);\n"
        "/* DualSense touch point from ScePadData.touchData.touch[0].\n"
        " * Accepted coordinates: 0..1919 x 0..1079; false means no live finger.\n"
        " * Both are best-effort: invalid report layouts leave them inert. */\n"
        "bool radio_input_touch(unsigned short *x, unsigned short *y);\n"
        "void radio_input_lightbar(int r, int g, int b);\n"
        "unsigned long long radio_input_milliseconds(void);",
    )
    input_cpp = worktree / "src" / "radio_input.cpp"
    replace_once(
        input_cpp,
        '#include "radio_input.hpp"',
        '#include "radio_input.hpp"\n#include <stdio.h>',
    )
    replace_once(
        input_cpp,
        "queue[queue_write] = (radio_input_event_t){key, pressed};",
        "queue[queue_write] = (radio_input_event_t){key, pressed, false, 0, 0};",
    )
    replace_once(
        input_cpp,
        "    extern uint64_t SDL_GetTicks64(void);",
        "    extern uint64_t SDL_GetTicks64(void);\n"
        "    extern int scePadSetLightBar(int32_t handle, const void *param);",
    )
    replace_once(
        input_cpp,
        "static int32_t pad_handle = -1;",
        "static int32_t pad_handle = -1;\n"
        "static unsigned short touch_x = 0, touch_y = 0;\n"
        "static bool touch_finger = false;\n"
        "static bool touch_valid = false;\n"
        "static bool pad_click_down = false;\n"
        "static uint64_t touch_sample_at;",
    )
    replace_once(
        input_cpp,
        "static void update_analog_action(",
        "static void queue_touch_click(bool pressed, bool contact, unsigned short x, unsigned short y)\n"
        "{\n"
        "    queue_push(RADIO_INPUT_PAD_CLICK, pressed);\n"
        "    const unsigned latest = (queue_write + INPUT_QUEUE_SIZE - 1U) % INPUT_QUEUE_SIZE;\n"
        "    queue[latest].touch_contact = contact;\n"
        "    queue[latest].touch_x = x;\n"
        "    queue[latest].touch_y = y;\n"
        "}\n\n"
        "static void update_analog_action(",
    )
    replace_once(
        input_cpp,
        "    button_state = current;\n",
        "    button_state = current;\n"
        "    touch_finger = false;\n"
        "    /* ScePadData.touchData starts at 0x34; first touch x/y/id at 0x3c/0x3e/0x40. */\n"
        "    if (sample[0x34] != 0)\n"
        "    {\n"
        "        const unsigned short px = (unsigned short)(sample[0x3c] | ((unsigned)sample[0x3d] << 8));\n"
        "        const unsigned short py = (unsigned short)(sample[0x3e] | ((unsigned)sample[0x3f] << 8));\n"
        "        if ((sample[0x40] & 0x80U) == 0 && px < 1920 && py < 1080)\n"
        "        {\n"
        "            touch_x = px;\n"
        "            touch_y = py;\n"
        "            touch_finger = true;\n"
        "            touch_valid = true;\n"
        "            touch_sample_at = monotonic_milliseconds();\n"
        "        }\n"
        "    }\n"
        "    const bool click_down = !neutral && (current & UINT32_C(0x00100000)) != 0;\n"
        "    if (click_down != pad_click_down)\n"
        "    {\n"
        "        queue_touch_click(click_down, touch_finger, touch_x, touch_y);\n"
        "        pad_click_down = click_down;\n"
        "    }\n",
    )
    replace_once(
        input_cpp,
        "void radio_input_shutdown(void)\n{",
        "unsigned long long radio_input_milliseconds(void)\n"
        "{\n"
        "    return SDL_GetTicks64();\n"
        "}\n"
        "\n"
        "bool radio_input_touch(unsigned short *x, unsigned short *y)\n"
        "{\n"
        "    if (!touch_valid || !touch_finger ||\n"
        "        monotonic_milliseconds() - touch_sample_at > UINT64_C(120))\n"
        "        return false;\n"
        "    if (x)\n"
        "        *x = touch_x;\n"
        "    if (y)\n"
        "        *y = touch_y;\n"
        "    return touch_finger;\n"
        "}\n"
        "\n"
        "void radio_input_lightbar(int r, int g, int b)\n"
        "{\n"
        "    if (pad_handle < 0)\n"
        "        return;\n"
        "    const unsigned char param[4] = {(unsigned char)r, (unsigned char)g,\n"
        "                                    (unsigned char)b, 0};\n"
        "    const int result = scePadSetLightBar(pad_handle, param);\n"
        "    if (result < 0)\n"
        "        fprintf(stderr, \"[ProsperoRadio][lightbar] scePadSetLightBar failed result=%d\\n\", result);\n"
        "}\n"
        "\n"
        "void radio_input_shutdown(void)\n{\n    radio_input_lightbar(0, 0, 0);",
    )
    reset_marker = (
        "    left_stick_key = -1;\n    left_stick_repeat_at = 0;\n"
        "    right_stick_key = -1;\n    right_stick_repeat_at = 0;"
    )
    input_text = input_cpp.read_text(encoding="utf-8")
    if input_text.count(reset_marker) != 2:
        raise RuntimeError("Expected input state reset in init and shutdown")
    input_cpp.write_text(
        input_text.replace(
            reset_marker,
            reset_marker
            + "\n    pad_click_down = false;\n    touch_finger = false;\n"
            + "    touch_valid = false;\n    touch_sample_at = 0;",
        ),
        encoding="utf-8",
    )

    # ---- service: 12-band biquad EQ and independent L/R trims -------------
    service_hpp = worktree / "include" / "radio_service.hpp"
    replace_once(
        service_hpp,
        "bool radio_service_toggle_favorite(unsigned station_index);",
        "/* Graphic equalizer: twelve bands and independent stereo trims. */\n"
        "void radio_service_eq_set_gain(int band, int gain_db);\n"
        "int radio_service_eq_gain(int band);\n"
        "void radio_service_eq_set_channel_gain(int channel, int gain_db);\n"
        "int radio_service_eq_channel_gain(int channel);\n"
        "void radio_service_eq_preset(int preset); /* 0 flat, 1 rock, 2 pop, 3 jazz */\n"
        "int radio_service_eq_preset(void); /* -1 when the current band gains are custom */\n"
        "/* AUX ingest server state for the AUX/BARRIDO surfaces. */\n"
        "void radio_service_aux_start(void);\n"
        "bool radio_service_aux_running(void);\n"
        "int radio_service_aux_stations(void);\n"
        "bool radio_service_toggle_favorite(unsigned station_index);",
    )
    service_cpp = worktree / "src" / "radio_service.cpp"
    replace_once(
        service_cpp,
        "#define AUDIO_OUT_VOLUME_0DB 0x8000",
        "#define AUDIO_OUT_VOLUME_0DB 0x8000\n"
        "\n"
        "/* --- 12-band graphic equalizer (RBJ biquads, in-place 16-bit) ------ */\n"
        "#define EQ_BANDS 12\n"
        "typedef struct { float b0, b1, b2, a1, a2, z1, z2; } eq_band_state_t;\n"
        "static eq_band_state_t g_eq_l[EQ_BANDS];\n"
        "static eq_band_state_t g_eq_r[EQ_BANDS];\n"
        "static std::atomic<int> g_eq_gain_db[EQ_BANDS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};\n"
        "static std::atomic<int> g_eq_channel_gain_db[2] = {0, 0};\n"
        "static float g_eq_limiter_gain[2] = {1.0f, 1.0f};\n"
        "static std::atomic<int> g_eq_preset{0};\n"
        "static std::atomic<bool> g_eq_dirty{true};\n"
        "static const float g_eq_freq[EQ_BANDS] = {60.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 1500.0f,\n"
        "    2000.0f, 3000.0f, 4000.0f, 6000.0f, 8000.0f, 12000.0f};\n"
        "static const int g_eq_presets[4][EQ_BANDS] = {\n"
        "    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},\n"
        "    {5, 4, 3, 1, -1, 0, 1, 3, 3, 4, 4, 4},\n"
        "    {2, 2, 1, 2, 2, 2, 2, 1, 0, 1, 1, 1},\n"
        "    {3, 1, -1, 1, 2, 2, 2, 2, 2, 2, 3, 3},\n"
        "};\n"
        "void radio_service_eq_process(int16_t *frames, unsigned count);\n"
        "\n"
        "static void eq_design_band(int band, eq_band_state_t *out)\n"
        "{\n"
        "    const float pi = 3.14159265f;\n"
        "    const float a = powf(10.0f, g_eq_gain_db[band].load(std::memory_order_relaxed) / 40.0f);\n"
        "    const float w0 = 2.0f * pi * g_eq_freq[band] / 48000.0f;\n"
        "    const float cw = cosf(w0), sw = sinf(w0);\n"
        "    const float alpha = sw / (2.0f * 0.7071f);\n"
        "    float b0, b1, b2, a0, a1, a2;\n"
        "    if (band == 0)\n"
        "    {\n"
        "        const float sq = 2.0f * sqrtf(a) * alpha;\n"
        "        b0 = a * ((a + 1) - (a - 1) * cw + sq);\n"
        "        b1 = 2 * a * ((a - 1) - (a + 1) * cw);\n"
        "        b2 = a * ((a + 1) - (a - 1) * cw - sq);\n"
        "        a0 = (a + 1) + (a - 1) * cw + sq;\n"
        "        a1 = -2 * ((a - 1) + (a + 1) * cw);\n"
        "        a2 = (a + 1) + (a - 1) * cw - sq;\n"
        "    }\n"
        "    else if (band == EQ_BANDS - 1)\n"
        "    {\n"
        "        const float sq = 2.0f * sqrtf(a) * alpha;\n"
        "        b0 = a * ((a + 1) + (a - 1) * cw + sq);\n"
        "        b1 = -2 * a * ((a - 1) + (a + 1) * cw);\n"
        "        b2 = a * ((a + 1) + (a - 1) * cw - sq);\n"
        "        a0 = (a + 1) - (a - 1) * cw + sq;\n"
        "        a1 = 2 * ((a - 1) - (a + 1) * cw);\n"
        "        a2 = (a + 1) - (a - 1) * cw - sq;\n"
        "    }\n"
        "    else\n"
        "    {\n"
        "        b0 = 1 + alpha * a; b1 = -2 * cw; b2 = 1 - alpha * a;\n"
        "        a0 = 1 + alpha / a; a1 = -2 * cw; a2 = 1 - alpha / a;\n"
        "    }\n"
        "    out->b0 = b0 / a0; out->b1 = b1 / a0; out->b2 = b2 / a0;\n"
        "    out->a1 = a1 / a0; out->a2 = a2 / a0;\n"
        "}\n"
        "\n"
        "void radio_service_eq_set_gain(int band, int gain_db)\n"
        "{\n"
        "    if (band < 0 || band >= EQ_BANDS)\n"
        "        return;\n"
        "    gain_db = gain_db < -12 ? -12 : gain_db > 12 ? 12 : gain_db;\n"
        "    if (g_eq_gain_db[band].load(std::memory_order_relaxed) != gain_db)\n"
        "    {\n"
        "        g_eq_gain_db[band].store(gain_db, std::memory_order_relaxed);\n"
        "        g_eq_dirty.store(true, std::memory_order_release);\n"
        "        g_eq_preset.store(-1, std::memory_order_relaxed);\n"
        "        for (int preset = 0; preset < 4; ++preset)\n"
        "        {\n"
        "            bool matches = true;\n"
        "            for (int index = 0; index < EQ_BANDS; ++index)\n"
        "                matches = matches && g_eq_gain_db[index].load(std::memory_order_relaxed) == g_eq_presets[preset][index];\n"
        "            if (matches)\n"
        "            {\n"
        "                g_eq_preset.store(preset, std::memory_order_relaxed);\n"
        "                break;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "int radio_service_eq_gain(int band)\n"
        "{\n"
        "    return band >= 0 && band < EQ_BANDS ? g_eq_gain_db[band].load(std::memory_order_relaxed) : 0;\n"
        "}\n"
        "\n"
        "void radio_service_eq_set_channel_gain(int channel, int gain_db)\n"
        "{\n"
        "    if (channel < 0 || channel > 1)\n"
        "        return;\n"
        "    gain_db = gain_db < -12 ? -12 : gain_db > 12 ? 12 : gain_db;\n"
        "    g_eq_channel_gain_db[channel].store(gain_db, std::memory_order_relaxed);\n"
        "}\n"
        "\n"
        "int radio_service_eq_channel_gain(int channel)\n"
        "{\n"
        "    return channel >= 0 && channel < 2 ? g_eq_channel_gain_db[channel].load(std::memory_order_relaxed) : 0;\n"
        "}\n"
        "\n"
        "void radio_service_eq_preset(int preset)\n"
        "{\n"
        "    if (preset < 0 || preset > 3)\n"
        "        return;\n"
        "    for (int i = 0; i < EQ_BANDS; ++i)\n"
        "        g_eq_gain_db[i].store(g_eq_presets[preset][i], std::memory_order_relaxed);\n"
        "    g_eq_preset.store(preset, std::memory_order_relaxed);\n"
        "    g_eq_dirty.store(true, std::memory_order_release);\n"
        "}\n"
        "\n"
        "int radio_service_eq_preset(void)\n{\n    return g_eq_preset.load(std::memory_order_relaxed);\n}\n"
        "\n"
        "void radio_service_eq_process(int16_t *frames, unsigned count)\n"
        "{\n"
        "    const float channel_gain_l = powf(10.0f, g_eq_channel_gain_db[0].load(std::memory_order_relaxed) / 20.0f);\n"
        "    const float channel_gain_r = powf(10.0f, g_eq_channel_gain_db[1].load(std::memory_order_relaxed) / 20.0f);\n"
        "    if (g_eq_dirty.exchange(false, std::memory_order_acq_rel))\n"
        "    {\n"
        "        for (int i = 0; i < EQ_BANDS; ++i)\n"
        "        {\n"
        "            const float right_z1 = g_eq_r[i].z1, right_z2 = g_eq_r[i].z2;\n"
        "            eq_design_band(i, &g_eq_l[i]);\n"
        "            g_eq_r[i] = g_eq_l[i];\n"
        "            g_eq_r[i].z1 = right_z1; g_eq_r[i].z2 = right_z2;\n"
        "        }\n"
        "    }\n"
        "    for (unsigned f = 0; f < count; ++f)\n"
        "    {\n"
        "        float l = frames[f * 2];\n"
        "        float r = frames[f * 2 + 1];\n"
        "        for (int b = 0; b < EQ_BANDS; ++b)\n"
        "        {\n"
        "            eq_band_state_t *s = &g_eq_l[b];\n"
        "            const float yl = s->b0 * l + s->z1;\n"
        "            s->z1 = s->b1 * l - s->a1 * yl + s->z2;\n"
        "            s->z2 = s->b2 * l - s->a2 * yl;\n"
        "            l = yl;\n"
        "            s = &g_eq_r[b];\n"
        "            const float yr = s->b0 * r + s->z1;\n"
        "            s->z1 = s->b1 * r - s->a1 * yr + s->z2;\n"
        "            s->z2 = s->b2 * r - s->a2 * yr;\n"
        "            r = yr;\n"
        "        }\n"
        "        l *= channel_gain_l;\n"
        "        r *= channel_gain_r;\n"
        "        const float peak_l = fabsf(l), peak_r = fabsf(r);\n"
        "        const float target_l = peak_l > 30000.0f ? 30000.0f / peak_l : 1.0f;\n"
        "        const float target_r = peak_r > 30000.0f ? 30000.0f / peak_r : 1.0f;\n"
        "        if (target_l < g_eq_limiter_gain[0]) g_eq_limiter_gain[0] = target_l;\n"
        "        else g_eq_limiter_gain[0] += (1.0f - g_eq_limiter_gain[0]) * 0.0005f;\n"
        "        if (target_r < g_eq_limiter_gain[1]) g_eq_limiter_gain[1] = target_r;\n"
        "        else g_eq_limiter_gain[1] += (1.0f - g_eq_limiter_gain[1]) * 0.0005f;\n"
        "        l *= g_eq_limiter_gain[0];\n"
        "        r *= g_eq_limiter_gain[1];\n"
        "        frames[f * 2] = (int16_t)(l < -32768.0f ? -32768 : l > 32767.0f ? 32767 : l);\n"
        "        frames[f * 2 + 1] = (int16_t)(r < -32768.0f ? -32768 : r > 32767.0f ? 32767 : r);\n"
        "    }\n"
        "}\n"
        "\n"
        "/* --- v027: AUX ingest server delegated to the payload bridge -------- */\n"
        "static int g_aux_stations = -1;\n"
        "void radio_service_aux_start(void) {}\n"
        "bool radio_service_aux_running(void) { return false; }\n"
        "int radio_service_aux_stations(void) { return g_aux_stations; }\n"
        "void radio_service_eq_process(int16_t *frames, unsigned count);",
    )
    replace_once(
        service_cpp,
        "        memcpy(block, sink->queue_blocks + index * AUDIO_OUT_GRAIN * 2U, sizeof(block));",
        "        memcpy(block, sink->queue_blocks + index * AUDIO_OUT_GRAIN * 2U, sizeof(block));\n"
        "        radio_service_eq_process(block, AUDIO_OUT_GRAIN);",
    )
    replace_once(
        service_cpp,
        "#include \"pcm_queue.hpp\"",
        "#include \"pcm_queue.hpp\"\n#include <atomic>\n#include <errno.h>\n#include <math.h>\n#include <string.h>\n#include <stdio.h>\n#include <stdlib.h>",
    )


def patch_aux_server(worktree: Path) -> None:
    """Delegate AUX lifecycle and file transfer to the named payload RPC API."""
    service_cpp = worktree / "src" / "radio_service.cpp"
    service_hpp = worktree / "include" / "radio_service.hpp"
    replace_once(
        service_hpp,
        "int radio_service_aux_stations(void);",
        "int radio_service_aux_stations(void);\n"
        "void radio_service_aux_stations_set(int count);",
    )
    text = service_cpp.read_text(encoding="utf-8")
    start_marker = "/* --- v027: AUX ingest server delegated to the payload bridge -------- */"
    end_marker = "void radio_service_eq_process(int16_t *frames, unsigned count);"
    if text.count(start_marker) != 1:
        raise RuntimeError("Expected exactly one generated AUX server block")
    start = text.index(start_marker)
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError("Generated AUX server block has no EQ boundary")
    end += len(end_marker)
    replacement = r"""static int g_aux_stations = -1;

void radio_service_aux_start(void)
{
    const bool ready = radio_payload_bridge_aux_start();
    fprintf(stderr, "[AUX] payload server start %s port=7000\n", ready ? "PASS" : "FAIL");
}

bool radio_service_aux_running(void)
{
    return radio_payload_bridge_aux_running();
}

int radio_service_aux_stations(void)
{
    return g_aux_stations;
}

void radio_service_aux_stations_set(int count)
{
    if (count < 0)
        count = -1;
    else if (count > 4096)
        count = 4096;
    g_aux_stations = count;
}

void radio_service_eq_process(int16_t *frames, unsigned count);"""
    service_cpp.write_text(text[:start] + replacement + text[end:], encoding="utf-8")

def patch_catalog_paging(worktree: Path) -> None:
    """Resolve global list indices against the exact active catalog view."""
    service_cpp = worktree / "src" / "radio_service.cpp"
    service_hpp = worktree / "include" / "radio_service.hpp"
    replace_once(
        service_hpp,
        "bool radio_service_get_station(unsigned index, radio_station_t * out_station);",
        "bool radio_service_get_station(unsigned index, radio_station_t * out_station);\n"
        "bool radio_service_get_station_by_uuid(const char *uuid, radio_station_t *out_station);\n"
        "bool radio_service_play_uuid(const char *uuid);\n"
        "bool radio_service_play_external(const radio_station_t *station);",
    )
    store_hpp = worktree / "include" / "radio_catalog_store.hpp"
    replace_once(
        store_hpp,
        "size_t radio_catalog_store_query_stations(\n",
        "bool radio_catalog_store_get_station_by_uuid(radio_catalog_store_t *store,\n"
        "                                             const char *uuid,\n"
        "                                             radio_station_t *out_station);\n"
        "size_t radio_catalog_store_query_stations(\n",
    )
    store_cpp = worktree / "src" / "radio_catalog_store.cpp"
    replace_once(
        store_cpp,
        "size_t radio_catalog_store_query_stations(radio_catalog_store_t *store,",
        """bool radio_catalog_store_get_station_by_uuid(radio_catalog_store_t *store,
                                                   const char *uuid,
                                                   radio_station_t *out_station)
{
    if (store == nullptr || uuid == nullptr || *uuid == '\\0' || out_station == nullptr)
        return false;
    sqlite3_stmt *statement = nullptr;
    if (!prepare(store, "SELECT " STATION_COLUMNS " FROM stations s WHERE s.uuid=?1 LIMIT 1",
                 &statement) ||
        sqlite3_bind_text(statement, 1, uuid, -1, SQLITE_TRANSIENT) != SQLITE_OK)
    {
        sqlite3_finalize(statement);
        return false;
    }
    const int result = sqlite3_step(statement);
    const bool found = result == SQLITE_ROW;
    if (found)
        read_station(statement, out_station);
    store->error = result == SQLITE_DONE || result == SQLITE_ROW ? SQLITE_OK : result;
    sqlite3_finalize(statement);
    return found;
}

size_t radio_catalog_store_query_stations(radio_catalog_store_t *store,""",
    )
    replace_once(
        service_cpp,
        "static unsigned g_station_count;",
        "static unsigned g_station_count;\n"
        "static unsigned g_station_page_offset;\n"
        "static radio_catalog_query_t g_view_query;\n"
        "static bool g_view_query_enabled;\n"
        "static radio_catalog_order_t g_view_order = RADIO_CATALOG_ORDER_POPULAR;\n"
        "static bool g_view_favorites_only;",
    )
    replace_function(
        service_cpp,
        "bool radio_service_get_station(unsigned index, radio_station_t *out_station)",
        """bool radio_service_get_station(unsigned index, radio_station_t *out_station)
{
    if (out_station == nullptr)
        return false;
    SDL_LockMutex(g_state_mutex);
    const bool found = index >= g_station_page_offset &&
                       index - g_station_page_offset < g_station_count;
    const radio_catalog_query_t query = g_view_query;
    const bool query_enabled = g_view_query_enabled;
    const radio_catalog_order_t order = g_view_order;
    const bool favorites_only = g_view_favorites_only;
    if (found)
        *out_station = g_stations[index - g_station_page_offset];
    SDL_UnlockMutex(g_state_mutex);
    if (found)
        return true;
    if (g_store_mutex == nullptr)
        return false;
    SDL_LockMutex(g_store_mutex);
    radio_station_t fetched{};
    const size_t loaded = radio_catalog_store_query_stations(
        &g_catalog_store, query_enabled ? &query : nullptr, order, favorites_only, index,
        &fetched, 1U);
    const int store_error = radio_catalog_store_error(&g_catalog_store);
    SDL_UnlockMutex(g_store_mutex);
    if (store_error != 0 || loaded == 0U)
        return false;
    *out_station = fetched;
    return true;
}""",
    )
    replace_once(
        service_cpp,
        "bool radio_service_query_page(const radio_catalog_query_t *query, radio_catalog_order_t order,",
        """bool radio_service_get_station_by_uuid(const char *uuid, radio_station_t *out_station)
{
    if (uuid == nullptr || *uuid == '\\0' || out_station == nullptr || g_store_mutex == nullptr)
        return false;
    SDL_LockMutex(g_store_mutex);
    const bool found = radio_catalog_store_get_station_by_uuid(&g_catalog_store, uuid, out_station);
    const int store_error = radio_catalog_store_error(&g_catalog_store);
    SDL_UnlockMutex(g_store_mutex);
    return found && store_error == 0;
}

bool radio_service_query_page(const radio_catalog_query_t *query, radio_catalog_order_t order,""",
    )
    replace_once(
        service_cpp,
        "g_station_count = (unsigned)loaded;\n    g_status.station_count = g_station_count;",
        "g_station_count = (unsigned)loaded;\n"
        "    g_station_page_offset = offset;\n"
        "    g_view_query = query != nullptr ? *query : radio_catalog_query_t{};\n"
        "    g_view_query_enabled = query != nullptr;\n"
        "    g_view_order = order;\n"
        "    g_view_favorites_only = favorites_only;\n"
        "    g_status.station_count = (unsigned)total;",
    )
    replace_once(
        service_cpp,
        "            g_status.playing_index = (unsigned)playing;",
        "            g_status.playing_index = offset + (unsigned)playing;",
    )
    replace_function(
        service_cpp,
        "bool radio_service_station_is_playing(unsigned index)",
        """bool radio_service_station_is_playing(unsigned index)
{
    radio_station_t station{};
    if (!radio_service_get_station(index, &station))
        return false;
    SDL_LockMutex(g_state_mutex);
    const bool active = g_status.playback_state != RADIO_PLAYBACK_STOPPED &&
                        g_status.playback_state != RADIO_PLAYBACK_ERROR;
    const bool playing = active && g_have_playing_station &&
                         strcmp(station.uuid, g_playing_station.uuid) == 0;
    SDL_UnlockMutex(g_state_mutex);
    return playing;
}""",
    )
    replace_function(
        service_cpp,
        "void radio_service_play(unsigned station_index)",
        """static bool start_station_playback(const radio_station_t *source, unsigned station_index)
{
    if (source == nullptr)
        return false;
    if (SDL_AtomicGet(&g_playback_running))
    {
        SDL_LockMutex(g_state_mutex);
        const bool finishing = g_status.playback_state == RADIO_PLAYBACK_STOPPED ||
                               g_status.playback_state == RADIO_PLAYBACK_ERROR;
        SDL_UnlockMutex(g_state_mutex);
        if (finishing)
        {
            while (SDL_AtomicGet(&g_playback_running))
                SDL_Delay(1);
        }
        else
        {
            radio_service_stop();
            return false;
        }
    }
    auto *station = static_cast<radio_station_t *>(malloc(sizeof(radio_station_t)));
    if (station == nullptr)
        return false;
    *station = *source;
    SDL_LockMutex(g_state_mutex);
    g_playing_station = *station;
    g_have_playing_station = true;
    g_status.playing_index = station_index;
    g_status.playback_state = RADIO_PLAYBACK_CONNECTING;
    g_status.sample_rate = 0;
    g_status.channels = 0;
    g_status.error_code = 0;
    SDL_UnlockMutex(g_state_mutex);

    SDL_AtomicSet(&g_stop_playback, 0);
    SDL_AtomicSet(&g_playback_running, 1);
    void *thread = nullptr;
    if (scePthreadCreate(&thread, nullptr, playback_thread, station, "radio-audio") != 0)
    {
        SDL_AtomicSet(&g_playback_running, 0);
        free(station);
        SDL_LockMutex(g_state_mutex);
        g_have_playing_station = false;
        memset(&g_playing_station, 0, sizeof(g_playing_station));
        SDL_UnlockMutex(g_state_mutex);
        set_playback_state(RADIO_PLAYBACK_ERROR, -1, 0, 0);
        return false;
    }
    scePthreadDetach(thread);
    return true;
}

void radio_service_play(unsigned station_index)
{
    radio_station_t station{};
    if (radio_service_get_station(station_index, &station))
        (void)start_station_playback(&station, station_index);
}

bool radio_service_play_uuid(const char *uuid)
{
    radio_station_t station{};
    return radio_service_get_station_by_uuid(uuid, &station) &&
           start_station_playback(&station, UINT_MAX);
}

bool radio_service_play_external(const radio_station_t *station)
{
    if (station == nullptr || station->url[0] == '\\0' || station->uuid[0] == '\\0')
        return false;
    return start_station_playback(station, UINT_MAX);
}""",
    )


def patch_payload_persistence(worktree: Path) -> None:
    """Persist cache/preferences through the private payload filesystem bridge."""
    service_cpp = worktree / "src" / "radio_service.cpp"
    replace_once(
        service_cpp,
        '#include "radio_catalog_store.hpp"',
        '#include "radio_catalog_store.hpp"\n#include "payload_probe.hpp"',
    )

    store_hpp = worktree / "include" / "radio_catalog_store.hpp"
    replace_once(
        store_hpp,
        "bool radio_catalog_store_set_favorite(radio_catalog_store_t * store,\n"
        "                                      const char * uuid, bool favorite);",
        "bool radio_catalog_store_set_favorite(radio_catalog_store_t * store,\n"
        "                                      const char * uuid, bool favorite);\n"
        "bool radio_catalog_store_clear_favorites(radio_catalog_store_t * store);",
    )
    store_cpp = worktree / "src" / "radio_catalog_store.cpp"
    replace_once(
        store_cpp,
        "size_t radio_catalog_store_favorite_count(radio_catalog_store_t *store)",
        "bool radio_catalog_store_clear_favorites(radio_catalog_store_t * store)\n"
        "{\n"
        "    return store != nullptr && execute(store, \"DELETE FROM favorites\");\n"
        "}\n\n"
        "size_t radio_catalog_store_favorite_count(radio_catalog_store_t *store)",
    )
    replace_once(
        service_cpp,
        """    if (load_favorites_file())
    {
        for (unsigned i = 0U; i < g_favorite_count; ++i)
            radio_catalog_store_set_favorite(&g_catalog_store, g_favorites[i], true);
        return true;
    }""",
        """    if (load_favorites_file())
    {
        /* The durable file is authoritative, including removals and an empty list. */
        if (!radio_catalog_store_clear_favorites(&g_catalog_store))
            return false;
        for (unsigned i = 0U; i < g_favorite_count; ++i)
            radio_catalog_store_set_favorite(&g_catalog_store, g_favorites[i], true);
        return true;
    }""",
    )
    replace_once(
        service_cpp,
        "static void *refresh_thread(void *task_data)",
        """static bool persist_catalog_snapshot_to_payload(void)
{
    const char *path = \"/download0/radio-browser-persist.tmp.sqlite3\";
    unlink(path);
    radio_catalog_store_t snapshot = {};
    SDL_LockMutex(g_store_mutex);
    const bool opened = radio_catalog_store_open(&snapshot, path);
    const bool copied = opened && radio_catalog_store_backup(&snapshot, &g_catalog_store) &&
                        radio_catalog_store_integrity_check(&snapshot);
    radio_catalog_store_close(&snapshot);
    SDL_UnlockMutex(g_store_mutex);
    const bool stored = copied &&
                        radio_payload_bridge_push_file(RADIO_PAYLOAD_CATALOG, path);
    unlink(path);
    fprintf(stderr, \"[ProsperoRadio][persistence] catalog snapshot %s\\n\",
            stored ? \"stored in /data/radio\" : \"bridge copy failed\");
    return stored;
}

static void *refresh_thread(void *task_data)""",
    )
    replace_once(
        service_cpp,
        """            if (task.full_sync)
                load_facet_snapshots();
            SDL_LockMutex(g_state_mutex);""",
        """            if (task.full_sync)
            {
                load_facet_snapshots();
                (void)persist_catalog_snapshot_to_payload();
            }
            SDL_LockMutex(g_state_mutex);""",
    )
    replace_function(
        service_cpp,
        "bool radio_service_toggle_favorite(unsigned station_index)",
        """bool radio_service_toggle_favorite(unsigned station_index)
{
    radio_station_t station{};
    if (!radio_service_get_station(station_index, &station))
        return false;
    const char *uuid = station.uuid;
    SDL_LockMutex(g_state_mutex);
    const bool now_favorite = !favorite_unlocked(uuid);
    const bool have_capacity = !now_favorite || ensure_favorite_capacity(g_favorite_count + 1U);
    SDL_UnlockMutex(g_state_mutex);
    if (!have_capacity)
        return false;

    SDL_LockMutex(g_state_mutex);
    if (now_favorite)
    {
        if (!favorite_unlocked(uuid))
            SDL_strlcpy(g_favorites[g_favorite_count++], uuid, sizeof(g_favorites[0]));
    }
    else
    {
        for (unsigned i = 0U; i < g_favorite_count; ++i)
        {
            if (strcmp(g_favorites[i], uuid) != 0)
                continue;
            if (i + 1U < g_favorite_count)
                memmove(g_favorites[i], g_favorites[i + 1U],
                        (g_favorite_count - i - 1U) * sizeof(g_favorites[0]));
            --g_favorite_count;
            break;
        }
    }
    SDL_UnlockMutex(g_state_mutex);
    if (!save_favorites_file())
        return false;
    SDL_LockMutex(g_store_mutex);
    const bool store_updated =
        radio_catalog_store_set_favorite(&g_catalog_store, uuid, now_favorite);
    SDL_UnlockMutex(g_store_mutex);
    if (!store_updated)
        return false;
    (void)radio_payload_bridge_push(RADIO_PAYLOAD_FAVORITES);
    return now_favorite;
}""",
    )


def patch_controller_input(worktree: Path) -> None:
    input_header = worktree / "include" / "radio_input.hpp"
    replace_once(
        input_header,
        "    RADIO_INPUT_RIGHT,\n    RADIO_INPUT_COUNT",
        "    RADIO_INPUT_RIGHT,\n    RADIO_INPUT_VOLUME_UP,\n    RADIO_INPUT_VOLUME_DOWN,\n"
        "    RADIO_INPUT_STATION_PREVIOUS,\n    RADIO_INPUT_STATION_NEXT,\n    RADIO_INPUT_COUNT",
    )

    input_cpp = worktree / "src" / "radio_input.cpp"
    replace_once(
        input_cpp,
        "#define STICK_REPEAT_DELAY_MS UINT64_C(350)\n#define STICK_REPEAT_MS UINT64_C(110)",
        "#define STICK_REPEAT_DELAY_MS UINT64_C(350)\n"
        "#define STICK_REPEAT_MS UINT64_C(140)\n"
        "#define TUNING_REPEAT_DELAY_MS UINT64_C(420)\n"
        "#define TUNING_REPEAT_MS UINT64_C(260)",
    )
    replace_once(
        input_cpp,
        "static int analog_key = -1;\nstatic uint64_t analog_repeat_at;",
        "static int left_stick_key = -1;\n"
        "static uint64_t left_stick_repeat_at;\n"
        "static int right_stick_key = -1;\n"
        "static uint64_t right_stick_repeat_at;",
    )
    replace_function(
        input_cpp,
        "static int stick_direction(uint8_t x, uint8_t y)",
        """static int left_stick_action(uint8_t y)
{
    if (y < STICK_LOW)
        return RADIO_INPUT_VOLUME_UP;
    if (y > STICK_HIGH)
        return RADIO_INPUT_VOLUME_DOWN;
    return -1;
}

static int right_stick_action(uint8_t x)
{
    if (x < STICK_LOW)
        return RADIO_INPUT_STATION_PREVIOUS;
    if (x > STICK_HIGH)
        return RADIO_INPUT_STATION_NEXT;
    return -1;
}""",
    )
    replace_once(
        input_cpp,
        "static void process_sample(const unsigned char *sample)",
        """static void update_analog_action(int current, int *previous, uint64_t *repeat_at,
                                  uint64_t now, uint64_t repeat_delay)
{
    if (current == *previous)
        return;
    if (*previous >= 0)
        queue_push((radio_input_key_t)*previous, false);
    *previous = current;
    if (current >= 0)
    {
        queue_push((radio_input_key_t)current, true);
        *repeat_at = now + repeat_delay;
    }
}

static void repeat_analog_action(int action, uint64_t *repeat_at, uint64_t now,
                                 uint64_t repeat_period)
{
    if (action < 0 || now < *repeat_at)
        return;
    queue_push((radio_input_key_t)action, true);
    *repeat_at = now + repeat_period;
}

static void process_sample(const unsigned char *sample)""",
    )
    replace_function(
        input_cpp,
        "static void process_sample(const unsigned char *sample)",
        """static void process_sample(const unsigned char *sample)
{
    uint32_t current;
    memcpy(&current, sample, sizeof(current));
    const bool neutral = sample[76] == 0 || (current & PAD_BUTTON_INTERCEPTED) != 0;
    if (neutral)
        current = 0;

    const uint32_t changed = button_state ^ current;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
    {
        if ((changed & buttons[i].button) != 0)
            queue_push(buttons[i].key, (current & buttons[i].button) != 0);
    }
    button_state = current;

    const uint64_t now = monotonic_milliseconds();
    const int volume_action = neutral ? -1 : left_stick_action(sample[5]);
    const int station_action = neutral ? -1 : right_stick_action(sample[6]);
    update_analog_action(volume_action, &left_stick_key, &left_stick_repeat_at, now,
                         STICK_REPEAT_DELAY_MS);
    update_analog_action(station_action, &right_stick_key, &right_stick_repeat_at, now,
                         TUNING_REPEAT_DELAY_MS);
}""",
    )
    old_poll = """void radio_input_poll(void)
{
    if (pad_handle < 0)
        return;
    const int count = scePadRead(pad_handle, samples, PAD_SAMPLE_CAPACITY);
    for (int i = 0; i < count; ++i)
        process_sample(samples[i]);
    if (analog_key >= 0)
    {
        const uint64_t now = monotonic_milliseconds();
        if (now >= analog_repeat_at)
        {
            queue_push((radio_input_key_t)analog_key, true);
            analog_repeat_at = now + STICK_REPEAT_MS;
        }
    }
}"""
    new_poll = """void radio_input_poll(void)
{
    if (pad_handle < 0)
        return;
    const int count = scePadRead(pad_handle, samples, PAD_SAMPLE_CAPACITY);
    for (int i = 0; i < count; ++i)
        process_sample(samples[i]);
    const uint64_t now = monotonic_milliseconds();
    repeat_analog_action(left_stick_key, &left_stick_repeat_at, now, STICK_REPEAT_MS);
    repeat_analog_action(right_stick_key, &right_stick_repeat_at, now, TUNING_REPEAT_MS);
}"""
    replace_once(input_cpp, old_poll, new_poll)

    old_reset = "    analog_key = -1;\n    analog_repeat_at = 0;"
    new_reset = (
        "    left_stick_key = -1;\n    left_stick_repeat_at = 0;\n"
        "    right_stick_key = -1;\n    right_stick_repeat_at = 0;"
    )
    text = input_cpp.read_text(encoding="utf-8")
    if text.count(old_reset) != 2:
        raise RuntimeError("Expected input state resets in init and shutdown")
    input_cpp.write_text(text.replace(old_reset, new_reset), encoding="utf-8")


def patch_audio_service(worktree: Path) -> None:
    service_header = worktree / "include" / "radio_service.hpp"
    replace_once(
        service_header,
        "void radio_service_stop(void);",
        "void radio_service_stop(void);\n"
        "void radio_service_set_volume(unsigned volume_percent);\n"
        "unsigned radio_service_get_volume(void);\n"
        "unsigned radio_service_get_favorite_count(void);",
    )

    service_cpp = worktree / "src" / "radio_service.cpp"
    replace_once(
        service_cpp,
        "static radio_service_status_t g_status;\n",
        "static radio_service_status_t g_status;\nstatic SDL_atomic_t g_volume_percent = {100};\n",
    )
    replace_once(
        service_cpp,
        "static int sink_audio_thread(void *argument)\n{",
        """static void sink_apply_volume(audio_sink_t *sink, int percent)
{
    if (sink == nullptr || sink->handle < 0)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    const int level = (AUDIO_OUT_VOLUME_0DB * percent) / 100;
    int volumes[8];
    for (int &volume : volumes)
        volume = level;
    sceAudioOutSetVolume(sink->handle, 3, volumes);
}

static int sink_audio_thread(void *argument)
{""",
    )
    replace_once(
        service_cpp,
        "    bool started = false;\n    bool played = false;\n",
        "    bool started = false;\n    bool played = false;\n    int applied_volume = -1;\n",
    )
    replace_once(
        service_cpp,
        "        const int result = sceAudioOutOutput(sink->handle, block);",
        "        const int requested_volume = SDL_AtomicGet(&g_volume_percent);\n"
        "        if (requested_volume != applied_volume)\n"
        "        {\n"
        "            sink_apply_volume(sink, requested_volume);\n"
        "            applied_volume = requested_volume;\n"
        "        }\n"
        "        const int result = sceAudioOutOutput(sink->handle, block);",
    )
    replace_once(
        service_cpp,
        "    int volumes[8];\n    for (unsigned i = 0; i < 8; ++i)\n        volumes[i] = AUDIO_OUT_VOLUME_0DB;\n"
        "    sceAudioOutSetVolume(sink->handle, 3, volumes);",
        "    sink_apply_volume(sink, SDL_AtomicGet(&g_volume_percent));",
    )
    replace_once(
        service_cpp,
        "bool radio_service_query_page(const radio_catalog_query_t *query, radio_catalog_order_t order,",
        """unsigned radio_service_get_favorite_count(void)
{
    if (g_store_mutex == nullptr)
        return 0U;
    radio_catalog_query_t query{};
    SDL_LockMutex(g_store_mutex);
    const size_t count = radio_catalog_store_query_count(&g_catalog_store, &query, true);
    const int error = radio_catalog_store_error(&g_catalog_store);
    SDL_UnlockMutex(g_store_mutex);
    return error == 0 && count <= UINT_MAX ? static_cast<unsigned>(count) : 0U;
}

bool radio_service_query_page(const radio_catalog_query_t *query, radio_catalog_order_t order,""",
    )
    replace_once(
        service_cpp,
        "void radio_service_stop(void)",
        """void radio_service_set_volume(unsigned volume_percent)
{
    if (volume_percent > 100U)
        volume_percent = 100U;
    SDL_AtomicSet(&g_volume_percent, static_cast<int>(volume_percent));
}

unsigned radio_service_get_volume(void)
{
    const int volume = SDL_AtomicGet(&g_volume_percent);
    return volume < 0 ? 0U : static_cast<unsigned>(volume);
}

void radio_service_stop(void)""",
    )


def patch_stream_codec_detection(worktree: Path) -> None:
    """Choose a direct-stream decoder from HTTP metadata or bounded sniffing.

    M3U URLs often omit file extensions (for example RNE's /mp3/high paths),
    and treating every unknown URL as AAC silently sends MP3 streams to the
    wrong decoder. Keep the initial probe bounded and replay its bytes.
    """
    service_cpp = worktree / "src" / "radio_service.cpp"
    replace_once(
        service_cpp,
        '            accept = "application/vnd.apple.mpegurl, application/x-mpegURL, "\n'
        '                     "video/mp2t, audio/aac, audio/aacp, */*";\n',
        '            accept = "application/vnd.apple.mpegurl, application/x-mpegURL, "\n'
        '                     "video/mp2t, audio/mpeg, audio/mp3, audio/aac, audio/aacp, */*";\n',
    )
    replace_once(
        service_cpp,
        "static size_t http_icy_metadata_interval(int request)\n"
        "{\n"
        "    char *headers = nullptr;\n"
        "    size_t size = 0U;\n"
        "    if (sceHttpGetAllResponseHeaders(request, &headers, &size) < 0 || headers == nullptr)\n"
        "        return 0U;\n"
        "    return icy_metadata_interval_from_headers(headers, size);\n"
        "}\n",
        "static size_t http_icy_metadata_interval(int request)\n"
        "{\n"
        "    char *headers = nullptr;\n"
        "    size_t size = 0U;\n"
        "    if (sceHttpGetAllResponseHeaders(request, &headers, &size) < 0 || headers == nullptr)\n"
        "        return 0U;\n"
        "    return icy_metadata_interval_from_headers(headers, size);\n"
        "}\n"
        "\n"
        "static const char *http_audio_codec(int request)\n"
        "{\n"
        "    char *headers = nullptr;\n"
        "    size_t size = 0U;\n"
        "    if (sceHttpGetAllResponseHeaders(request, &headers, &size) < 0 || headers == nullptr)\n"
        "        return nullptr;\n"
        "    static const char key[] = \"content-type:\";\n"
        "    constexpr size_t key_size = sizeof(key) - 1U;\n"
        "    for (size_t i = 0U; i + key_size <= size; ++i)\n"
        "    {\n"
        "        if (strncasecmp(headers + i, key, key_size) != 0)\n"
        "            continue;\n"
        "        size_t begin = i + key_size;\n"
        "        while (begin < size && (headers[begin] == ' ' || headers[begin] == '\\t'))\n"
        "            ++begin;\n"
        "        size_t end = begin;\n"
        "        while (end < size && headers[end] != ';' && headers[end] != '\\r' &&\n"
        "               headers[end] != '\\n' && headers[end] != ' ' && headers[end] != '\\t')\n"
        "            ++end;\n"
        "        const size_t length = end - begin;\n"
        "        const char *type = headers + begin;\n"
        "        if ((length == 10U && strncasecmp(type, \"audio/mpeg\", length) == 0) ||\n"
        "            (length == 9U && strncasecmp(type, \"audio/mp3\", length) == 0) ||\n"
        "            (length == 12U && strncasecmp(type, \"audio/x-mpeg\", length) == 0))\n"
        "            return \"MP3\";\n"
        "        if ((length == 9U && strncasecmp(type, \"audio/aac\", length) == 0) ||\n"
        "            (length == 10U && strncasecmp(type, \"audio/aacp\", length) == 0) ||\n"
        "            (length == 11U && strncasecmp(type, \"audio/x-aac\", length) == 0))\n"
        "            return \"AAC\";\n"
        "        if ((length == 9U && strncasecmp(type, \"audio/ogg\", length) == 0) ||\n"
        "            (length == 15U && strncasecmp(type, \"application/ogg\", length) == 0))\n"
        "            return \"OGG\";\n"
        "        if ((length == 10U && strncasecmp(type, \"audio/flac\", length) == 0) ||\n"
        "            (length == 12U && strncasecmp(type, \"audio/x-flac\", length) == 0))\n"
        "            return \"FLAC\";\n"
        "        return nullptr;\n"
        "    }\n"
        "    return nullptr;\n"
        "}\n",
    )

    replace_once(
        service_cpp,
        "static size_t find_adts(const uint8_t *data, size_t size)\n"
        "{\n"
        "    for (size_t i = 0; i + 1U < size; ++i)\n"
        "    {\n"
        "        if (data[i] == 0xffU && (data[i + 1U] & 0xf6U) == 0xf0U)\n"
        "            return i;\n"
        "    }\n"
        "    return size;\n"
        "}\n",
        "static size_t find_adts(const uint8_t *data, size_t size)\n"
        "{\n"
        "    for (size_t i = 0; i + 1U < size; ++i)\n"
        "    {\n"
        "        if (data[i] == 0xffU && (data[i + 1U] & 0xf6U) == 0xf0U)\n"
        "            return i;\n"
        "    }\n"
        "    return size;\n"
        "}\n"
        "\n"
        "enum auto_stream_format_t\n"
        "{\n"
        "    AUTO_STREAM_MP3 = 10,\n"
        "    AUTO_STREAM_AAC = 11,\n"
        "    AUTO_STREAM_FLAC = 12,\n"
        "    AUTO_STREAM_UNSUPPORTED = -16\n"
        "};\n"
        "\n"
        "static int prefixed_http_open_auto(prefixed_http_reader_t *reader,\n"
        "                                   stream_read_fn read_stream, void *read_context)\n"
        "{\n"
        "    memset(reader, 0, sizeof(*reader));\n"
        "    reader->read_stream = read_stream;\n"
        "    reader->read_context = read_context;\n"
        "    while (reader->prefix_size < sizeof(reader->prefix) &&\n"
        "           !SDL_AtomicGet(&g_stop_playback))\n"
        "    {\n"
        "        if (reader->prefix_size >= 4U &&\n"
        "            memcmp(reader->prefix, \"fLaC\", 4U) == 0)\n"
        "            return AUTO_STREAM_FLAC;\n"
        "        const ogg_format_t ogg = ogg_probe(reader->prefix, reader->prefix_size);\n"
        "        if (ogg > OGG_FORMAT_NEED_MORE)\n"
        "            return static_cast<int>(ogg);\n"
        "        mp3_header_t mp3_header{};\n"
        "        if (mp3_header_find(reader->prefix, reader->prefix_size, &mp3_header) <\n"
        "            reader->prefix_size)\n"
        "            return AUTO_STREAM_MP3;\n"
        "        if (find_adts(reader->prefix, reader->prefix_size) < reader->prefix_size)\n"
        "            return AUTO_STREAM_AAC;\n"
        "        const int received = read_stream(read_context, reader->prefix + reader->prefix_size,\n"
        "                                         sizeof(reader->prefix) - reader->prefix_size);\n"
        "        if (received < 0)\n"
        "            return received;\n"
        "        if (received == 0)\n"
        "            return -3;\n"
        "        reader->prefix_size += static_cast<size_t>(received);\n"
        "    }\n"
        "    return AUTO_STREAM_UNSUPPORTED;\n"
        "}\n",
    )

    replace_once(
        service_cpp,
        "    int result;\n"
        "    if (strcasecmp(station->codec, \"OPUS\") == 0)\n"
        "        result = play_opus_reader(direct_read, direct_context, output_frames);\n"
        "    else if (strcasecmp(station->codec, \"VORBIS\") == 0)\n"
        "        result = play_vorbis_reader(direct_read, direct_context, output_frames);\n"
        "    else if (strcasecmp(station->codec, \"FLAC\") == 0)\n"
        "    {\n"
        "        prefixed_http_reader_t reader;\n"
        "        const int format = prefixed_http_open(&reader, direct_read, direct_context);\n"
        "        if (format == OGG_FORMAT_FLAC)\n"
        "            result = play_ogg_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else\n"
        "            result = play_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "    }\n"
        "    else if (strcasecmp(station->codec, \"OGG\") == 0)\n"
        "    {\n"
        "        prefixed_http_reader_t reader;\n"
        "        const int format = prefixed_http_open(&reader, direct_read, direct_context);\n"
        "        if (format == OGG_FORMAT_OPUS)\n"
        "            result = play_opus_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == OGG_FORMAT_VORBIS)\n"
        "            result = play_vorbis_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == OGG_FORMAT_FLAC)\n"
        "            result = play_ogg_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else\n"
        "            result = format < 0 ? format : VORBIS_DECODER_UNSUPPORTED;\n"
        "    }\n"
        "    else\n"
        "    {\n"
        "        const bool mp3 = strcasecmp(station->codec, \"MP3\") == 0;\n"
        "        result =\n"
        "            play_audiodec_reader(direct_read, direct_context, source_channels, mp3, output_frames);\n"
        "    }",
        "    const char *response_codec = http_audio_codec(request);\n"
        "    const char *codec = response_codec != nullptr ? response_codec : station->codec;\n"
        "    int result;\n"
        "    if (strcasecmp(codec, \"OPUS\") == 0)\n"
        "        result = play_opus_reader(direct_read, direct_context, output_frames);\n"
        "    else if (strcasecmp(codec, \"VORBIS\") == 0)\n"
        "        result = play_vorbis_reader(direct_read, direct_context, output_frames);\n"
        "    else if (strcasecmp(codec, \"FLAC\") == 0)\n"
        "    {\n"
        "        prefixed_http_reader_t reader;\n"
        "        const int format = prefixed_http_open(&reader, direct_read, direct_context);\n"
        "        if (format == OGG_FORMAT_FLAC)\n"
        "            result = play_ogg_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else\n"
        "            result = play_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "    }\n"
        "    else if (strcasecmp(codec, \"OGG\") == 0)\n"
        "    {\n"
        "        prefixed_http_reader_t reader;\n"
        "        const int format = prefixed_http_open(&reader, direct_read, direct_context);\n"
        "        if (format == OGG_FORMAT_OPUS)\n"
        "            result = play_opus_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == OGG_FORMAT_VORBIS)\n"
        "            result = play_vorbis_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == OGG_FORMAT_FLAC)\n"
        "            result = play_ogg_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else\n"
        "            result = format < 0 ? format : VORBIS_DECODER_UNSUPPORTED;\n"
        "    }\n"
        "    else if (strcasecmp(codec, \"AUTO\") == 0 || strcasecmp(codec, \"STREAM\") == 0)\n"
        "    {\n"
        "        prefixed_http_reader_t reader;\n"
        "        const int format = prefixed_http_open_auto(&reader, direct_read, direct_context);\n"
        "        if (format == OGG_FORMAT_OPUS)\n"
        "            result = play_opus_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == OGG_FORMAT_VORBIS)\n"
        "            result = play_vorbis_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == OGG_FORMAT_FLAC)\n"
        "            result = play_ogg_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == AUTO_STREAM_FLAC)\n"
        "            result = play_flac_reader(prefixed_http_read, &reader, output_frames);\n"
        "        else if (format == AUTO_STREAM_MP3 || format == AUTO_STREAM_AAC)\n"
        "            result = play_audiodec_reader(prefixed_http_read, &reader, source_channels,\n"
        "                                          format == AUTO_STREAM_MP3, output_frames);\n"
        "        else\n"
        "            result = format < 0 ? format : AUTO_STREAM_UNSUPPORTED;\n"
        "    }\n"
        "    else\n"
        "    {\n"
        "        const bool mp3 = strcasecmp(codec, \"MP3\") == 0;\n"
        "        result =\n"
        "            play_audiodec_reader(direct_read, direct_context, source_channels, mp3, output_frames);\n"
        "    }",
    )


def apply_ui_overlay(worktree: Path, overlay: Path) -> None:
    # The physical-radio frontend ships complete overlay sources for
    # radio_app.cpp/.hpp; upstream files are replaced wholesale further down.
    for relative in ("assets/ui/main.rml", "assets/ui/styles/app.rcss"):
        src = overlay / relative
        dst = worktree / relative
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    for texture_name in ("radio_front_4k.ktx2", "radio_front_hybrid_4k.ktx2"):
        src = overlay / "assets/ui/art" / texture_name
        if not src.is_file():
            if texture_name == "radio_front_hybrid_4k.ktx2":
                continue
            raise FileNotFoundError(f"Missing required Vulkan backplate: {src}")
        dst = worktree / "assets/ui/art" / texture_name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    # Keep modular control atlases in the materialized upstream asset tree so
    # the package step includes them even before the app binds their UV frames.
    control_source = overlay / "assets/ui/controls"
    control_destination = worktree / "assets/ui/controls"
    for source_asset in sorted(control_source.iterdir()):
        if source_asset.suffix.lower() not in {".tga", ".json"}:
            continue
        destination_asset = control_destination / source_asset.name
        destination_asset.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_asset, destination_asset)


def apply_stream_protocols(worktree: Path, overlay: Path) -> None:
    """Stage the bounded HLS AES-128 and static DASH AAC implementation."""
    modules = (
        "include/radio_aes128.hpp",
        "include/radio_dash.hpp",
        "src/radio_aes128.cpp",
        "src/radio_dash.cpp",
    )
    for relative in modules:
        source = overlay / relative
        if not source.is_file():
            raise RuntimeError(f"Missing stream protocol source: {source}")
        destination = worktree / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    patch = overlay / "patches" / "radio-stream-protocols.patch"
    if not patch.is_file():
        raise RuntimeError(f"Missing stream protocol patch: {patch}")
    check = subprocess.run(
        ["git", "apply", "--check", str(patch)],
        cwd=worktree,
        capture_output=True,
        text=True,
    )
    if check.returncode != 0:
        raise RuntimeError(
            "Stream protocol patch no longer matches the pinned source:\n"
            + check.stderr.strip()
        )
    subprocess.run(
        ["git", "apply", str(patch)], cwd=worktree, check=True
    )

    service = (worktree / "src/radio_service.cpp").read_text(encoding="utf-8")
    expected = (
        '#include "radio_aes128.hpp"',
        '#include "radio_dash.hpp"',
        "#define STREAM_OPEN_DASH 2",
        "static int dash_reader_open(",
    )
    missing = [marker for marker in expected if marker not in service]
    if missing:
        raise RuntimeError(f"Stream protocol integration incomplete: {', '.join(missing)}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: apply-vulkan.py <upstream-worktree>", file=sys.stderr)
        return 2
    worktree = Path(sys.argv[1]).resolve()
    overlay = Path(__file__).resolve().parent
    if not (worktree / ".git").exists():
        raise SystemExit(f"Not a Git worktree: {worktree}")
    head = run("git", "rev-parse", "HEAD", cwd=worktree)
    if head != UPSTREAM_SHA and head != UPSTREAM_SHA_CURRENT:
        print(f"WARNING: Upstream revision {head} differs from expected {UPSTREAM_SHA}", file=sys.stderr)
        print(f"         Proceeding anyway (compatibility mode)", file=sys.stderr)

    apply_ui_overlay(worktree, overlay)
    for relative in (
        "include/payload_probe.hpp",
        "include/radio_disc_protocol.h",
        "include/radio_usb_protocol.h",
        "src/payload_probe.cpp",
    ):
        source = overlay / relative
        destination = worktree / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    patch_controller_input(worktree)
    patch_v027_radio_features(worktree)
    patch_aux_server(worktree)
    patch_catalog_paging(worktree)
    patch_payload_persistence(worktree)
    patch_audio_service(worktree)
    patch_stream_codec_detection(worktree)
    apply_stream_protocols(worktree, overlay)
    # The physical-radio frontend ships complete sources: replace the upstream
    # application layer wholesale instead of anchoring patches to it.
    shutil.copy2(overlay / "include/radio_app.hpp", worktree / "include/radio_app.hpp")
    shutil.copy2(overlay / "src/radio_app.cpp", worktree / "src/radio_app.cpp")
    patch_cpp_runtime(worktree)
    patch_console_ux(worktree, overlay, VERSION)

    main_cpp = worktree / "src" / "main.cpp"
    text = main_cpp.read_text(encoding="utf-8")
    replace_once(
        main_cpp,
        '#include "radio_input.hpp"',
        '#include "radio_input.hpp"\n#include "ps5_vulkan_renderer.hpp"\n'
        '\nextern "C" void ProsperoRuntimeLogMaintenance() noexcept;'
    )
    text = main_cpp.read_text(encoding="utf-8")
    text = replace_class(text, "SdlRenderInterface", 'class ProsperoVulkanRenderInterfaceAdapter final : public Rml::RenderInterfaceCompatibility\n{\n  public:\n    ProsperoVulkanRenderInterfaceAdapter()\n    {\n        backend_.Initialize("assets/ui/vulkan/ui.vert.spv", "assets/ui/vulkan/ui.frag.spv");\n    }\n\n    ~ProsperoVulkanRenderInterfaceAdapter() override = default;\n\n    void BeginFrame() { backend_.BeginFrame(); }\n    bool EndFrame() { return backend_.EndFrame(); }\n    bool IsInitialized() const { return backend_.IsInitialized(); }\n\n    void RenderGeometry(Rml::Vertex *vertices, int num_vertices, int *indices, int num_indices,\n                        Rml::TextureHandle texture, const Rml::Vector2f &translation) override\n    {\n        backend_.RenderGeometry(vertices, num_vertices, indices, num_indices, texture, translation);\n    }\n    bool LoadTexture(Rml::TextureHandle &handle, Rml::Vector2i &dimensions, const Rml::String &source) override\n    {\n        return backend_.LoadTexture(handle, dimensions, source);\n    }\n    bool GenerateTexture(Rml::TextureHandle &handle, const Rml::byte *source,\n                         const Rml::Vector2i &dimensions) override\n    {\n        return backend_.GenerateTexture(handle, source, dimensions);\n    }\n    void ReleaseTexture(Rml::TextureHandle texture) override { backend_.ReleaseTexture(texture); }\n    void EnableScissorRegion(bool enable) override { backend_.EnableScissorRegion(enable); }\n    void SetScissorRegion(int x, int y, int width, int height) override\n    {\n        backend_.SetScissorRegion(x, y, width, height);\n    }\n\n  private:\n    Ps5VulkanRenderInterface backend_;\n};')
    text = replace_runapp_body(text)
    text = remove_present_color(text)

    if "class SdlRenderInterface" in text or "SDL_CreateSoftwareRenderer" in text or "PresentColor(" in text:
        raise RuntimeError("SDL software rendering path was not fully removed")
    main_cpp.write_text(text, encoding="utf-8")

    remove_title_compat_duplicates(worktree)

    shutil.copy2(overlay / "src/ps5_vulkan_renderer.hpp", worktree / "src/ps5_vulkan_renderer.hpp")
    shutil.copy2(overlay / "src/ps5_vulkan_renderer.cpp", worktree / "src/ps5_vulkan_renderer.cpp")
    stage_overlay_tools(worktree, overlay)

    formatter = shutil.which("clang-format-18") or shutil.which("clang-format")
    if not formatter:
        raise RuntimeError("clang-format is required to format generated Vulkan C++ sources")
    subprocess.run(
        [
            formatter,
            "-i",
            str(main_cpp),
            str(worktree / "src/ps5_vulkan_renderer.hpp"),
            str(worktree / "src/ps5_vulkan_renderer.cpp"),
            str(worktree / "src/radio_input.cpp"),
            str(worktree / "src/radio_service.cpp"),
            str(worktree / "src/radio_app.cpp"),
            str(worktree / "src/payload_probe.cpp"),
            str(worktree / "src/radio_aes128.cpp"),
            str(worktree / "src/radio_dash.cpp"),
            str(worktree / "include/radio_input.hpp"),
            str(worktree / "include/radio_service.hpp"),
            str(worktree / "include/radio_app.hpp"),
            str(worktree / "include/payload_probe.hpp"),
            str(worktree / "include/radio_aes128.hpp"),
            str(worktree / "include/radio_dash.hpp"),
        ],
        cwd=worktree,
        check=True,
    )

    # Patch the link-input arrays first because the duplicate-symbol audit
    # deliberately consumes the Vulkan archive/object arrays created here.
    patch_target_build_driver(worktree)
    patch_native_weak_imports(worktree)
    patch_link_duplicate_audit(worktree)

    replace_once(
        worktree / "tools/run_clang_tidy.sh",
        '    -I"$root/vendor/ps5/sdl/include/SDL2" -I"$root/vendor/ps5/rmlui/include" \\\n    -isystem "$pacbrew")',
        '    -I"$root/vendor/ps5/sdl/include/SDL2" -I"$root/vendor/ps5/rmlui/include" \\\n    -I"$root/.local/vulkan/include" \\\n    -isystem "$pacbrew")',
    )
    replace_once(
        worktree / "tools/check_ui.py",
        '    assert "left: 240px;" in css and "width: 1440px;" in css',
        '    assert "left: 96px;" in css and "width: 1256px;" in css',
    )
    replace_once(
        worktree / "tools/check_ui.py",
        '    assert css.count("left: 140px;") >= 2',
        '    assert ".card-0 { left: 96px; top: 200px; }" in css\n'
        '    assert ".card-3 { left: 96px; top: 332px; }" in css',
    )
    replace_once(
        worktree / "tools/check_ui.py",
        '    assert "top: 52px;" in css',
        '    assert ".station-card { position: absolute; width: 408px; height: 120px;" in css',
    )
    replace_once(
        worktree / "tools/check_ui.py",
        '    "credits-overlay", "credits-close", "brand-mark", "brand-name", "brand-version",',
        '    "credits-overlay", "credits-close", "brand-mark", "brand-name", "brand-version",\n'
        '    "volume-level",',
    )
    replace_once(
        worktree / "tools/check_ui.py",
        '    assert "#credit-button.focused" in css and "#play-button.focused" in css',
        '    assert "#credit-button.focused" in css and "#play-button.focused" in css\n'
        '    input_source = (repo / "src/radio_input.cpp").read_text(encoding="utf-8")\n'
        '    app_source = (repo / "src/radio_app.cpp").read_text(encoding="utf-8")\n'
        '    assert "left_stick_action(sample[5])" in input_source\n'
        '    assert "right_stick_action(sample[6])" in input_source\n'
        '    assert "RADIO_INPUT_STATION_PREVIOUS" in app_source\n'
        '    assert "RADIO_INPUT_VOLUME_UP" in app_source',
    )

    param = worktree / "sce_sys" / "param.json"
    data = json.loads(param.read_text(encoding="utf-8"))
    if data.get("titleId") != "PPSA99001":
        raise RuntimeError("Unexpected Title ID; PPSA99001 is fixed")
    data["contentVersion"] = VERSION
    param.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    note = worktree / "VULKAN-INTEGRATION.md"
    note.write_text((overlay / "VULKAN-INTEGRATION.md").read_text(encoding="utf-8"), encoding="utf-8")
    print("ProsperoRadio Vulkan overlay applied successfully.")
    print(f"  Base commit : {UPSTREAM_SHA}")
    print(f"  Version     : {VERSION}")
    print("  Graphics    : RmlUi -> Vulkan 1.0 -> PS5_Vulkan -> AGC/VideoOut")
    print("  SDL         : input/time only; SDL software renderer removed from frame path")
    print("  UI          : 3-column x 2-row physical-radio redesign")
    print("  Controls    : left stick volume; right stick tuning; touch presets; AUX import on 7000")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
