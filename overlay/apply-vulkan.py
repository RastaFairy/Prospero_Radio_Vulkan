#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

UPSTREAM_SHA = "33898dd35375c1ae8370da137cfb6941d91c7684"
VERSION = "01.000.013"


def run(*args: str, cwd: Path) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"Expected exactly one occurrence in {path}, got {count}: {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")


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
    text = text.replace(link_marker, link_marker + vulkan_link, 1)

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


def apply_ui_overlay(worktree: Path, overlay: Path) -> None:
    include = worktree / "include" / "radio_app.hpp"
    replace_once(include,
                 "    static constexpr unsigned CardCount = 4;",
                 "    static constexpr unsigned CardCount = 6;")

    cpp = worktree / "src" / "radio_app.cpp"
    replace_once(cpp, "constexpr unsigned kFocusDiscover = 4;", "constexpr unsigned kFocusDiscover = 6;")
    replace_once(cpp, "constexpr unsigned kFocusPlay = 7;", "constexpr unsigned kFocusPlay = 9;")
    replace_once(cpp, "constexpr unsigned kFocusCredits = 8;", "constexpr unsigned kFocusCredits = 10;")

    old_navigation = """        if (key == RADIO_INPUT_CROSS)\n            TogglePlayback();\n        else if (key == RADIO_INPUT_LEFT && (slot & 1U))\n            focus_ = selected_slot_ = slot - 1;\n        else if (key == RADIO_INPUT_RIGHT)\n        {\n            if (!(slot & 1U) && card_stations_[slot + 1] != InvalidStation)\n                focus_ = selected_slot_ = slot + 1;\n            else\n                focus_ = kFocusPlay;\n        }\n        else if (key == RADIO_INPUT_UP)\n        {\n            if (slot >= 2)\n                focus_ = selected_slot_ = slot - 2;\n            else\n            {\n                ChangePage(-1, slot + 2);\n                return;\n            }\n        }\n        else if (key == RADIO_INPUT_DOWN)\n        {\n            if (slot < 2 && card_stations_[slot + 2] != InvalidStation)\n                focus_ = selected_slot_ = slot + 2;\n            else if (page_start_ + CardCount < visible_count_)\n            {\n                ChangePage(1, slot & 1U);\n                return;\n            }\n            else if (view_ == View::Discover)\n                focus_ = kFocusDiscover + slot % 3;\n            else\n                focus_ = kFocusPlay;\n        }"""
    new_navigation = """        if (key == RADIO_INPUT_CROSS)\n            TogglePlayback();\n        else if (key == RADIO_INPUT_LEFT)\n        {\n            if ((slot % 3U) != 0U)\n                focus_ = selected_slot_ = slot - 1U;\n        }\n        else if (key == RADIO_INPUT_RIGHT)\n        {\n            if ((slot % 3U) != 2U && card_stations_[slot + 1U] != InvalidStation)\n                focus_ = selected_slot_ = slot + 1U;\n            else\n                focus_ = kFocusPlay;\n        }\n        else if (key == RADIO_INPUT_UP)\n        {\n            const unsigned row = slot / 3U;\n            if (row > 0U && card_stations_[slot - 3U] != InvalidStation)\n                focus_ = selected_slot_ = slot - 3U;\n            else\n            {\n                ChangePage(-1, slot + 3U);\n                return;\n            }\n        }\n        else if (key == RADIO_INPUT_DOWN)\n        {\n            const unsigned row = slot / 3U;\n            if (row < 1U && card_stations_[slot + 3U] != InvalidStation)\n                focus_ = selected_slot_ = slot + 3U;\n            else if (page_start_ + CardCount < visible_count_)\n            {\n                ChangePage(1, slot % 3U);\n                return;\n            }\n            else if (view_ == View::Discover)\n                focus_ = kFocusDiscover + slot % 3U;\n            else\n                focus_ = kFocusPlay;\n        }"""
    replace_once(cpp, old_navigation, new_navigation)

    for relative in ("assets/ui/main.rml", "assets/ui/styles/app.rcss"):
        src = overlay / relative
        dst = worktree / relative
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    src = overlay / "assets/ui/art/radio_front_4k.ktx2"
    dst = worktree / "assets/ui/art/radio_front_4k.ktx2"
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


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

    # Patch the link-input arrays first because the duplicate-symbol audit
    # deliberately consumes the Vulkan archive/object arrays created here.
    patch_target_build_driver(worktree)
    patch_native_weak_imports(worktree)
    patch_link_duplicate_audit(worktree)

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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
