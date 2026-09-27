// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include <SDL2/SDL.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/RenderInterfaceCompatibility.h>
#include <RmlUi/Core/SystemInterface.h>

#include "bitmap_font_engine.hpp"
#include "radio_app.hpp"
#include "radio_ime.hpp"
#include "radio_input.hpp"
#include "ps5_vulkan_renderer.hpp"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <pthread.h>
#include <vector>

extern "C" int sceKernelUsleep(std::uint32_t microseconds);
extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" void *mmap(void *address, std::size_t length, int protection, int flags, int descriptor,
                      long offset);
extern "C" int munmap(void *address, std::size_t length);
extern "C" void *__dso_handle = nullptr;
extern "C" char __eh_frame_hdr_start[1] = {};
extern "C" char __eh_frame_hdr_end[1] = {};
extern "C" char __eh_frame_start[1] = {};
extern "C" char __eh_frame_end[1] = {};

namespace
{

constexpr std::size_t kMappedAllocationThreshold = 64 * 1024;
constexpr std::uint64_t kAllocationMagic = UINT64_C(0x524144494F4D454D);
constexpr int kProtectionReadWrite = 3;
constexpr int kMapPrivateAnonymous = 0x1002;

struct alignas(std::max_align_t) AllocationHeader
{
    std::uint64_t magic;
    std::size_t requested_size;
    std::size_t mapped_size;
};

void *AllocateTracked(std::size_t size)
{
    if (size == 0)
        size = 1;
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
        return nullptr;

    const std::size_t total = sizeof(AllocationHeader) + size;
    AllocationHeader *header = nullptr;
    std::size_t mapped_size = 0;
    if (size >= kMappedAllocationThreshold)
    {
        mapped_size = (total + 0x3fff) & ~std::size_t(0x3fff);
        void *mapping =
            mmap(nullptr, mapped_size, kProtectionReadWrite, kMapPrivateAnonymous, -1, 0);
        if (mapping != reinterpret_cast<void *>(-1))
        {
            header = static_cast<AllocationHeader *>(mapping);
        }
    }
    else
    {
        header = static_cast<AllocationHeader *>(std::malloc(total));
    }
    if (!header)
        return nullptr;

    header->magic = kAllocationMagic;
    header->requested_size = size;
    header->mapped_size = mapped_size;
    return header + 1;
}

void FreeTracked(void *allocation) noexcept
{
    if (!allocation)
        return;
    auto *header = static_cast<AllocationHeader *>(allocation) - 1;
    // SDL can retain small allocations made by its original allocator before
    // custom memory functions are installed. Those remain libc-owned.
    if (header->magic != kAllocationMagic)
    {
        std::free(allocation);
        return;
    }
    if (header->mapped_size != 0)
    {
        munmap(header, header->mapped_size);
    }
    else
    {
        std::free(header);
    }
}

void *CallocTracked(std::size_t count, std::size_t size)
{
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
        return nullptr;
    const std::size_t total = count * size;
    void *allocation = AllocateTracked(total);
    if (allocation)
        std::memset(allocation, 0, total);
    return allocation;
}

void *ReallocTracked(void *allocation, std::size_t size)
{
    if (!allocation)
        return AllocateTracked(size);
    if (size == 0)
    {
        FreeTracked(allocation);
        return nullptr;
    }

    auto *old_header = static_cast<AllocationHeader *>(allocation) - 1;
    if (old_header->magic != kAllocationMagic)
        std::abort();
    void *replacement = AllocateTracked(size);
    if (!replacement)
        return nullptr;
    std::memcpy(replacement, allocation,
                old_header->requested_size < size ? old_header->requested_size : size);
    FreeTracked(allocation);
    return replacement;
}

} // namespace

extern "C" int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    constexpr int running = 2;
    int state = __atomic_load_n(&once_control->state, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_DONE_INIT)
        return 0;

    int expected = PTHREAD_NEEDS_INIT;
    if (__atomic_compare_exchange_n(&once_control->state, &expected, running, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        init_routine();
        __atomic_store_n(&once_control->state, PTHREAD_DONE_INIT, __ATOMIC_RELEASE);
        return 0;
    }

    while (__atomic_load_n(&once_control->state, __ATOMIC_ACQUIRE) != PTHREAD_DONE_INIT)
    {
        sceKernelUsleep(100);
    }
    return 0;
}

extern "C" float strtof(const char *value, char **end)
{
    return static_cast<float>(strtod(value, end));
}

extern "C" int fseek(std::FILE *file, long offset, int origin)
{
    return fseeko(file, offset, origin);
}

extern "C" long ftell(std::FILE *file)
{
    return static_cast<long>(ftello(file));
}

extern "C" char *strcasestr(const char *haystack, const char *needle)
{
    if (!*needle)
        return const_cast<char *>(haystack);
    for (; *haystack; ++haystack)
    {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n)
        {
            const char hc = *h >= 'A' && *h <= 'Z' ? static_cast<char>(*h + ('a' - 'A')) : *h;
            const char nc = *n >= 'A' && *n <= 'Z' ? static_cast<char>(*n + ('a' - 'A')) : *n;
            if (hc != nc)
                break;
            ++h;
            ++n;
        }
        if (!*n)
            return const_cast<char *>(haystack);
    }
    return nullptr;
}

namespace
{

class AppSystemInterface final : public Rml::SystemInterface
{
  public:
    double GetElapsedTime() override
    {
        return static_cast<double>(SDL_GetTicks64() - start_ticks_) / 1000.0;
    }

  private:
    Uint64 start_ticks_ = SDL_GetTicks64();
};

class AppFileInterface final : public Rml::FileInterface
{
  public:
    Rml::FileHandle Open(const Rml::String &path) override
    {
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (!file)
        {
            const Rml::String app_path = "/app0/" + path;
            file = std::fopen(app_path.c_str(), "rb");
        }
        return reinterpret_cast<Rml::FileHandle>(file);
    }

    void Close(Rml::FileHandle file) override
    {
        if (file)
            std::fclose(reinterpret_cast<std::FILE *>(file));
    }

    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override
    {
        return std::fread(buffer, 1, size, reinterpret_cast<std::FILE *>(file));
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override
    {
        return fseeko(reinterpret_cast<std::FILE *>(file), offset, origin) == 0;
    }

    size_t Tell(Rml::FileHandle file) override
    {
        return static_cast<size_t>(ftello(reinterpret_cast<std::FILE *>(file)));
    }
};

class ProsperoVulkanRenderInterfaceAdapter final : public Rml::RenderInterfaceCompatibility
{
  public:
    ProsperoVulkanRenderInterfaceAdapter()
    {
        backend_.Initialize("assets/ui/vulkan/ui.vert.spv", "assets/ui/vulkan/ui.frag.spv");
    }

    ~ProsperoVulkanRenderInterfaceAdapter() override = default;

    void BeginFrame()
    {
        backend_.BeginFrame();
    }
    bool EndFrame()
    {
        return backend_.EndFrame();
    }
    bool IsInitialized() const
    {
        return backend_.IsInitialized();
    }

    void RenderGeometry(Rml::Vertex *vertices, int num_vertices, int *indices, int num_indices,
                        Rml::TextureHandle texture, const Rml::Vector2f &translation) override
    {
        backend_.RenderGeometry(vertices, num_vertices, indices, num_indices, texture, translation);
    }
    bool LoadTexture(Rml::TextureHandle &handle, Rml::Vector2i &dimensions,
                     const Rml::String &source) override
    {
        return backend_.LoadTexture(handle, dimensions, source);
    }
    bool GenerateTexture(Rml::TextureHandle &handle, const Rml::byte *source,
                         const Rml::Vector2i &dimensions) override
    {
        return backend_.GenerateTexture(handle, source, dimensions);
    }
    void ReleaseTexture(Rml::TextureHandle texture) override
    {
        backend_.ReleaseTexture(texture);
    }
    void EnableScissorRegion(bool enable) override
    {
        backend_.EnableScissorRegion(enable);
    }
    void SetScissorRegion(int x, int y, int width, int height) override
    {
        backend_.SetScissorRegion(x, y, width, height);
    }

  private:
    Ps5VulkanRenderInterface backend_;
};
;

bool LoadFonts()
{
    static constexpr const char *kBitmapFonts[] = {
        "assets/ui/fonts/lvgl-bitmap/Montserrat-20.fnt",
        "assets/ui/fonts/lvgl-bitmap/Montserrat-24.fnt",
        "assets/ui/fonts/lvgl-bitmap/Montserrat-28.fnt",
        "assets/ui/fonts/lvgl-bitmap/Montserrat-32.fnt",
        "assets/ui/fonts/lvgl-bitmap/Montserrat-36.fnt",
        "assets/ui/fonts/lvgl-bitmap/Montserrat-40.fnt",
        "assets/ui/fonts/lvgl-bitmap/Montserrat-48.fnt",
    };
    for (const char *font : kBitmapFonts)
    {
        if (!Rml::LoadFontFace(font))
            return false;
    }
    static constexpr const char *kMultilingualFonts[] = {
        "assets/ui/fonts/lvgl-bitmap/multilingual/Radio-20.fnt",
        "assets/ui/fonts/lvgl-bitmap/multilingual/Radio-24.fnt",
        "assets/ui/fonts/lvgl-bitmap/multilingual/Radio-28.fnt",
        "assets/ui/fonts/lvgl-bitmap/multilingual/Radio-32.fnt",
    };
    for (const char *font : kMultilingualFonts)
    {
        if (!Rml::LoadFontFace(font))
            return false;
    }
    return true;
}

[[noreturn]] void KeepProcessAlive()
{
    for (;;)
        sceKernelUsleep(1000000);
}

bool RunApp()
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
}

} // namespace

int main()
{
    RunApp();
    KeepProcessAlive();
}
