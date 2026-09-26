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
VERSION = "01.000.022"

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
        context->Update();
        render_interface.BeginFrame();
        context->Render();
        if (!render_interface.EndFrame())
            running = false;
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


def patch_radio_app(worktree: Path) -> None:
    app_header = worktree / "include" / "radio_app.hpp"
    replace_once(
        app_header,
        "    bool search_open_ = false;\n",
        "    bool search_open_ = false;\n"
        "    bool settings_open_ = false;\n"
        "    unsigned settings_focus_ = 0;\n",
    )
    replace_once(
        app_header,
        "    void OpenCredits();\n    void CloseCredits();\n",
        "    void OpenCredits();\n    void CloseCredits();\n"
        "    void OpenSettings();\n    void CloseSettings();\n"
        "    void RefreshSettings(bool refresh_favorites = true);\n"
        "    void HandleSettingsKey(radio_input_key_t key);\n"
        "    void AdjustVolume(int direction);\n"
        "    void StepStation(int direction);\n",
    )

    app_cpp = worktree / "src" / "radio_app.cpp"
    replace_once(
        app_cpp,
        """    if (key == RADIO_INPUT_TRIANGLE)
    {
        OpenSearch(InvalidStation);
        return;
    }
""",
        """    if (key == RADIO_INPUT_TRIANGLE)
    {
        OpenSettings();
        return;
    }
    if (key == RADIO_INPUT_LEFT || key == RADIO_INPUT_RIGHT)
    {
        SetView(key == RADIO_INPUT_LEFT ? -1 : 1);
        return;
    }
        """,
    )
    replace_once(
        app_cpp,
        """    if (key == RADIO_INPUT_L1)
    {
        SetView(-1);
        return;
    }
    if (key == RADIO_INPUT_R1)
    {
        SetView(1);
        return;
    }
""",
        "",
    )
    replace_once(
        app_cpp,
        "Choose Country, Genre, or Language below - Triangle opens search",
        "Choose a filter below to find new stations",
    )
    replace_once(
        app_cpp,
        "void RadioApp::SetView(int direction)",
        """void RadioApp::OpenSettings()
{
    if (settings_open_ || search_open_ || credits_open_)
        return;
    settings_open_ = true;
    settings_focus_ = 0;
    SetVisible(document_, "settings-overlay", true);
    RefreshSettings();
    UpdateFocus();
}

void RadioApp::CloseSettings()
{
    if (!settings_open_)
        return;
    settings_open_ = false;
    SetVisible(document_, "settings-overlay", false);
    UpdateFocus();
}

void RadioApp::RefreshSettings(bool refresh_favorites)
{
    const unsigned volume = radio_service_get_volume();
    char text[96];
    std::snprintf(text, sizeof(text), "VOL %u%%", volume);
    SetText(document_, "volume-level", text);
    if (!settings_open_)
        return;

    static const char *view_names[] = {"Popular", "Trending", "Top rated", "Favorites", "Discover"};
    const unsigned view_index = static_cast<unsigned>(view_);
    SetText(document_, "settings-current-list",
            view_index < sizeof(view_names) / sizeof(view_names[0]) ? view_names[view_index]
                                                                     : "Radio lists");
    std::snprintf(text, sizeof(text), "%u stations in this list", visible_count_);
    SetText(document_, "settings-station-count", text);
    std::snprintf(text, sizeof(text), "%u%%", volume);
    SetText(document_, "settings-volume-value", text);
    SetPixelProperty(document_, "settings-volume-fill", "width",
                     static_cast<int>((390U * volume) / 100U));
    if (refresh_favorites)
    {
        std::snprintf(text, sizeof(text), "%u saved stations", radio_service_get_favorite_count());
        SetText(document_, "settings-favorites-count", text);
    }

    radio_service_status_t status{};
    radio_service_get_status(&status);
    SetText(document_, "settings-refresh-state", status.refreshing ? "Updating..." : "Ready");
    SetClass(document_, "settings-favorites", "focused", settings_focus_ == 0U);
    SetClass(document_, "settings-refresh", "focused", settings_focus_ == 1U);
}

void RadioApp::HandleSettingsKey(radio_input_key_t key)
{
    if (key == RADIO_INPUT_TRIANGLE || key == RADIO_INPUT_CIRCLE)
    {
        CloseSettings();
        return;
    }
    if (key == RADIO_INPUT_LEFT || key == RADIO_INPUT_RIGHT)
    {
        SetView(key == RADIO_INPUT_LEFT ? -1 : 1);
        RefreshSettings();
        return;
    }
    if (key == RADIO_INPUT_UP || key == RADIO_INPUT_DOWN)
    {
        settings_focus_ = settings_focus_ == 0U ? 1U : 0U;
        RefreshSettings();
        return;
    }
    if (key != RADIO_INPUT_CROSS)
        return;
    if (settings_focus_ == 0U)
    {
        CloseSettings();
        view_ = View::Favorites;
        page_start_ = selected_slot_ = focus_ = 0;
        RefreshAll();
        UpdateFocus();
        return;
    }
    radio_service_refresh();
    RefreshSettings();
}

void RadioApp::AdjustVolume(int direction)
{
    const int current = static_cast<int>(radio_service_get_volume());
    const int next = std::clamp(current + direction * 2, 0, 100);
    if (next != current)
        radio_service_set_volume(static_cast<unsigned>(next));
    RefreshSettings(false);
}

void RadioApp::StepStation(int direction)
{
    if (visible_count_ == 0U)
        return;
    unsigned current = page_start_ + selected_slot_;
    if (current >= visible_count_)
        current = 0U;
    const unsigned next = direction < 0
                              ? (current == 0U ? visible_count_ - 1U : current - 1U)
                              : (current + 1U == visible_count_ ? 0U : current + 1U);
    radio_service_status_t status{};
    radio_service_get_status(&status);
    const bool resume_playback = PlaybackActive(status.playback_state);
    page_start_ = (next / CardCount) * CardCount;
    selected_slot_ = next % CardCount;
    focus_ = selected_slot_;
    BuildVisibleList();
    RefreshCards();
    RefreshHeading();
    RefreshDetail();
    UpdateFocus();
    if (resume_playback && card_stations_[selected_slot_] != InvalidStation)
        TogglePlayback();
}

void RadioApp::SetView(int direction)""",
    )
    replace_function(
        app_cpp,
        "void RadioApp::HandleInput(const radio_input_event_t &event)",
        """void RadioApp::HandleInput(const radio_input_event_t &event)
{
    if (!event.pressed)
        return;
    if (event.key == RADIO_INPUT_VOLUME_UP || event.key == RADIO_INPUT_VOLUME_DOWN)
    {
        AdjustVolume(event.key == RADIO_INPUT_VOLUME_UP ? 1 : -1);
        return;
    }
    if (credits_open_)
    {
        if (event.key == RADIO_INPUT_CROSS || event.key == RADIO_INPUT_CIRCLE)
            CloseCredits();
        return;
    }
    if (settings_open_)
    {
        HandleSettingsKey(event.key);
        return;
    }
    if (search_open_)
    {
        HandleSearchKey(event.key);
        return;
    }
    if (event.key == RADIO_INPUT_STATION_PREVIOUS || event.key == RADIO_INPUT_STATION_NEXT)
    {
        StepStation(event.key == RADIO_INPUT_STATION_PREVIOUS ? -1 : 1);
        return;
    }
    HandleMainKey(event.key);
}""",
    )
    replace_once(
        app_cpp,
        "    UpdateEqualizer(status);\n    last_status_ = status;",
        "    UpdateEqualizer(status);\n"
        "    if (settings_open_ && (!have_last_status_ || status.refreshing != last_status_.refreshing ||\n"
        "                          status.catalog_generation != last_status_.catalog_generation))\n"
        "        RefreshSettings();\n"
        "    last_status_ = status;",
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

    # Cabinet finish themes (24-bit flat fronts)
    theme_source = overlay / "assets/ui/themes"
    theme_destination = worktree / "assets/ui/themes"
    for source_asset in sorted(theme_source.iterdir()):
        if source_asset.suffix.lower() != ".tga":
            continue
        theme_destination.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_asset, theme_destination / source_asset.name)

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


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: apply-vulkan.py <upstream-worktree>", file=sys.stderr)
        return 2
    worktree = Path(sys.argv[1]).resolve()
    overlay = Path(__file__).resolve().parent
    if not (worktree / ".git").exists():
        raise SystemExit(f"Not a Git worktree: {worktree}")
    head = run("git", "rev-parse", "HEAD", cwd=worktree)
    if head != UPSTREAM_SHA:
        raise SystemExit(f"Unexpected upstream revision {head}; expected {UPSTREAM_SHA}")

    apply_ui_overlay(worktree, overlay)
    patch_controller_input(worktree)
    patch_audio_service(worktree)
    # The physical-radio frontend ships complete sources: replace the upstream
    # application layer wholesale instead of anchoring patches to it.
    shutil.copy2(overlay / "include/radio_app.hpp", worktree / "include/radio_app.hpp")
    shutil.copy2(overlay / "src/radio_app.cpp", worktree / "src/radio_app.cpp")
    patch_cpp_runtime(worktree)
    patch_console_ux(worktree, overlay, VERSION)

    main_cpp = worktree / "src" / "main.cpp"
    text = main_cpp.read_text(encoding="utf-8")
    replace_once(main_cpp, '#include "radio_input.hpp"', '#include "radio_input.hpp"\n#include "ps5_vulkan_renderer.hpp"')
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
            str(worktree / "include/radio_input.hpp"),
            str(worktree / "include/radio_service.hpp"),
            str(worktree / "include/radio_app.hpp"),
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
        '    "volume-level", "settings-overlay", "settings-current-list", "settings-station-count",\n'
        '    "settings-volume-row", "settings-volume-value", "settings-volume-fill",\n'
        '    "settings-favorites", "settings-favorites-count", "settings-refresh", "settings-refresh-state",',
    )
    replace_once(
        worktree / "tools/check_ui.py",
        '    assert "#credit-button.focused" in css and "#play-button.focused" in css',
        '    assert "#credit-button.focused" in css and "#play-button.focused" in css\n'
        '    assert "#settings-overlay.hidden" in css and ".settings-action.focused" in css\n'
        '    input_source = (repo / "src/radio_input.cpp").read_text(encoding="utf-8")\n'
        '    app_source = (repo / "src/radio_app.cpp").read_text(encoding="utf-8")\n'
        '    assert "left_stick_action(sample[5])" in input_source\n'
        '    assert "right_stick_action(sample[6])" in input_source\n'
        '    assert "RADIO_INPUT_STATION_PREVIOUS" in app_source and "OpenSettings();" in app_source\n'
        '    assert "settings_open_" in app_source and "RADIO_INPUT_VOLUME_UP" in app_source',
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
    print("  Controls    : left stick volume; right stick tuning; D-pad lists; triangle settings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
