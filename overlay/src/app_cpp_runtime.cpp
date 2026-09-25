/*
 * ps5-native-app-boilerplate - Minimal target C++ allocation runtime.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bridges standard C++ allocation operators to the clean-room libc module
 * without introducing exceptions, RTTI, or the complete libc++ runtime.
 *
 * ProsperoRadio modernized fork (01.000.017). Applied by overlay/apply-vulkan.py.
 *
 * Two problems on the target console shaped this fork:
 *
 * 1. The default stderr of a title is discarded, so every renderer diagnostic
 *    written with fprintf(stderr, ...) was lost. The static initializer below
 *    redirects stderr into /download0/prospero-radio.log (the writable data
 *    mount the radio service already uses, exposed by the etaHEN FTP).
 *
 * 2. The libc heap cannot grow past a few MB in this launch configuration.
 *    Measured on hardware (klog 2026-09-25): a calloc(1, 1221859) issued by
 *    the font glyph table returns NULL ~0.6 s after launch, and the previous
 *    build died the same way on a 1.6 MB read. Small allocations keep working,
 *    so every allocation of kPoolMinBlock bytes or more is served by this
 *    title's own Direct-Memory pool (growing mappings, reuse cache, 512 MB
 *    ceiling) instead of the libc heap. The link wraps malloc/calloc/realloc/
 *    free/posix_memalign so the whole title uses it transparently; pointers
 *    that do not carry the pool magic are forwarded to the real libc
 *    allocator, so libc-internal allocations stay consistent.
 *
 * Pool block layout: each Direct-Memory mapping starts with a PoolHeader and
 * the caller pointer is header + kPoolHeader, so every block carries the magic
 * inside its own mapping and free() never dereferences foreign memory.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <atomic>

extern "C"
{
    /* Real libSceLibcInternal allocations, exposed by the linker --wrap flags. */
    void *__real_malloc(std::size_t size);
    void __real_free(void *address);
    void *__real_realloc(void *address, std::size_t size);
    void *__real_calloc(std::size_t count, std::size_t size);
    int __real_posix_memalign(void **address, std::size_t alignment, std::size_t size);

    /* Kernel Direct-Memory services (same constants the Vulkan driver uses). */
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                      std::size_t length, std::size_t alignment, int memory_type,
                                      std::int64_t *physical_address);
    int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                 std::int64_t physical_address, std::size_t alignment);

    /* Wrap implementations, defined at the bottom of this file. The runtime's
     * own paths call them directly: on the target the --wrap flags resolve
     * plain malloc/free references to these same functions. */
    void *__wrap_malloc(std::size_t size);
    void __wrap_free(void *address);
    void *__wrap_calloc(std::size_t count, std::size_t size);
    void *__wrap_realloc(void *address, std::size_t size);
    int __wrap_posix_memalign(void **address, std::size_t alignment, std::size_t size);
}

namespace
{

/* The console discards the default stderr of a title. /download0 is writable
 * by this title and fetchable over FTP, so point stderr there before anything
 * else runs. Rotated once it grows past 2 MB. */
const bool g_runtime_log_ready = []() {
    std::FILE *log = std::freopen("/download0/prospero-radio.log", "a", stderr);
    if (log == nullptr)
        return false;
    if (std::ftell(log) > 2u * 1024u * 1024u)
        log = std::freopen("/download0/prospero-radio.log", "w", stderr);
    std::fprintf(stderr, "[PS5-RT] runtime log ready (app_cpp_runtime fork 01.000.017)\n");
    return log != nullptr;
}();

constexpr std::size_t kAllocTrailSize = 32;
constexpr std::size_t kMaxReasonableAllocation = 1024ull * 1024ull * 1024ull;

std::size_t g_alloc_trail[kAllocTrailSize] = {};
std::size_t g_alloc_trail_index = 0;

void alloc_trail_push(std::size_t size) noexcept
{
    g_alloc_trail[g_alloc_trail_index % kAllocTrailSize] = size;
    ++g_alloc_trail_index;
}

/* --- Dynamic Direct-Memory pool for large allocations -------------------- */

constexpr std::size_t kPoolMinBlock = 256ull * 1024ull;            /* route threshold */
constexpr std::size_t kPoolMaxMapped = 512ull * 1024ull * 1024ull; /* ceiling */
constexpr std::size_t kPoolHeader = 64;
constexpr std::size_t kPoolMapAlign = 0x4000;                      /* 16 KiB, as the driver */
constexpr int kPoolMemType = 12;                                   /* general garlic */
constexpr int kPoolProtection = 0x33;                              /* RW, as the driver */
constexpr std::uint64_t kPoolMagic = 0x4D454D5052533031ULL;        /* "01PRPMEM" */

struct PoolHeader
{
    std::uint64_t magic;
    std::uint64_t size;        /* usable bytes from the returned pointer */
    std::uint64_t mapped_len;
    std::int64_t physical;
    PoolHeader *next_free;     /* cache chain while the block is free */
    PoolHeader *registry_next; /* ownership-registry chain */
};
static_assert(sizeof(PoolHeader) <= kPoolHeader, "pool header must fit its reserved bytes");

std::atomic_flag g_pool_lock = ATOMIC_FLAG_INIT;
std::size_t g_pool_mapped_total = 0;
std::size_t g_pool_dmem_size = 0;
bool g_pool_dmem_checked = false;
PoolHeader *g_pool_free_head = nullptr;

/* Ownership registry: an open-chained hash set of live pool headers, keyed by
 * the caller pointer. free()/realloc() MUST consult this instead of probing
 * bytes before a pointer: libc serves some large chunks with mmap, where
 * memory right before the pointer is unmapped and a naive probe would fault. */
constexpr std::size_t kPoolRegistryBuckets = 4096; /* power of two */
PoolHeader *g_pool_registry[kPoolRegistryBuckets] = {};

std::size_t pool_registry_index(const void *address)
{
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    return static_cast<std::size_t>((value >> 4) ^ (value >> 18)) & (kPoolRegistryBuckets - 1);
}

void pool_registry_add(PoolHeader *header)
{
    const std::size_t index = pool_registry_index(
        static_cast<std::byte *>(static_cast<void *>(header)) + kPoolHeader);
    header->registry_next = g_pool_registry[index];
    g_pool_registry[index] = header;
}

void pool_registry_remove(PoolHeader *header)
{
    const std::size_t index = pool_registry_index(
        static_cast<std::byte *>(static_cast<void *>(header)) + kPoolHeader);
    for (PoolHeader **it = &g_pool_registry[index]; *it != nullptr; it = &(*it)->registry_next)
    {
        if (*it == header)
        {
            *it = header->registry_next;
            header->registry_next = nullptr;
            return;
        }
    }
}

struct PoolGuard
{
    PoolGuard() { while (g_pool_lock.test_and_set(std::memory_order_acquire)) {} }
    ~PoolGuard() { g_pool_lock.clear(std::memory_order_release); }
    PoolGuard(const PoolGuard &) = delete;
    PoolGuard &operator=(const PoolGuard &) = delete;
};

std::size_t pool_direct_memory_size()
{
    if (!g_pool_dmem_checked)
    {
        g_pool_dmem_size = sceKernelGetDirectMemorySize();
        g_pool_dmem_checked = true;
        if (g_pool_dmem_size == 0)
            std::fprintf(stderr, "[PS5-RT] direct memory size is zero; pool disabled\n");
    }
    return g_pool_dmem_size;
}

/* Caller holds the pool lock. Returns a fresh mapped block or nullptr. */
PoolHeader *pool_map_new(std::size_t usable)
{
    const std::size_t map_len = (usable + kPoolHeader + kPoolMapAlign - 1) & ~(kPoolMapAlign - 1);
    if (map_len < kPoolHeader || g_pool_mapped_total + map_len > kPoolMaxMapped)
        return nullptr;
    const std::size_t dmem_size = pool_direct_memory_size();
    if (dmem_size == 0)
        return nullptr;

    std::int64_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(dmem_size), map_len,
                                      kPoolMapAlign, kPoolMemType, &physical) < 0)
        return nullptr;
    void *mapped = nullptr;
    if (sceKernelMapDirectMemory(&mapped, map_len, kPoolProtection, 0, physical, kPoolMapAlign) < 0)
    {
        /* The physical allocation cannot be returned to the kernel here; it
         * still counts against the ceiling so the cap stays honest. */
        g_pool_mapped_total += map_len;
        return nullptr;
    }
    g_pool_mapped_total += map_len;

    PoolHeader *header = static_cast<PoolHeader *>(mapped);
    header->magic = kPoolMagic;
    header->size = map_len - kPoolHeader;
    header->mapped_len = map_len;
    header->physical = physical;
    header->next_free = nullptr;
    return header;
}

/* Caller holds the pool lock. Best fit over the cached free blocks. */
PoolHeader *pool_take_from_cache(std::size_t map_len_needed)
{
    PoolHeader **best = nullptr;
    for (PoolHeader **it = &g_pool_free_head; *it != nullptr; it = &(*it)->next_free)
    {
        if ((*it)->mapped_len < map_len_needed)
            continue;
        if (best == nullptr || (*it)->mapped_len < (*best)->mapped_len)
            best = it;
    }
    if (best == nullptr)
        return nullptr;
    PoolHeader *block = *best;
    *best = block->next_free;
    block->next_free = nullptr;
    return block;
}

/* Large-allocation entry point. The returned pointer is header + kPoolHeader,
 * so it is 16 KiB aligned and satisfies any C++ alignment requirement. */
void *pool_alloc(std::size_t size)
{
    if (size > kPoolMaxMapped)
        return nullptr;
    const std::size_t map_len_needed = size + kPoolHeader;

    PoolGuard guard;
    PoolHeader *header = pool_take_from_cache(map_len_needed);
    if (header == nullptr)
        header = pool_map_new(size);
    if (header == nullptr)
        return nullptr;

    header->size = header->mapped_len - kPoolHeader;
    pool_registry_add(header);
    return static_cast<std::byte *>(static_cast<void *>(header)) + kPoolHeader;
}

/* Returns the pool header for a pointer served by the pool, else nullptr.
 * Ownership is resolved through the registry: foreign pointers (for example
 * libc chunks that were served with mmap) are never probed, so no byte before
 * a caller pointer is ever dereferenced. Every pool pointer is base + 64 with
 * a 16 KiB aligned base, so the cheap alignment filter rejects most foreign
 * pointers before the lock is taken. */
PoolHeader *pool_header_of(void *address)
{
    if ((reinterpret_cast<std::uintptr_t>(address) & (kPoolMapAlign - 1)) != kPoolHeader)
        return nullptr;
    PoolGuard guard;
    const std::size_t index = pool_registry_index(address);
    for (PoolHeader *it = g_pool_registry[index]; it != nullptr; it = it->registry_next)
    {
        if (static_cast<std::byte *>(static_cast<void *>(it)) + kPoolHeader == address)
            return it;
    }
    return nullptr;
}

void pool_release(PoolHeader *header)
{
    PoolGuard guard;
    pool_registry_remove(header);
    header->next_free = g_pool_free_head;
    g_pool_free_head = header;
}

/* Reporting when an allocation cannot be satisfied at all. */
[[noreturn]] void allocation_failure(std::size_t size) noexcept
{
    std::fprintf(stderr,
                 "[PS5-RT] FATAL: allocation of %llu bytes failed (limit=%llu). Recent allocations:",
                 static_cast<unsigned long long>(size),
                 static_cast<unsigned long long>(kMaxReasonableAllocation));
    const std::size_t recorded =
        g_alloc_trail_index < kAllocTrailSize ? g_alloc_trail_index : kAllocTrailSize;
    for (std::size_t i = 0; i < recorded; ++i)
    {
        const std::size_t slot = (g_alloc_trail_index - 1 - i) % kAllocTrailSize;
        std::fprintf(stderr, "%s%llu", i == 0 ? " " : ", ",
                     static_cast<unsigned long long>(g_alloc_trail[slot]));
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] void *allocate(std::size_t size) noexcept
{
    alloc_trail_push(size);
    if (size > kMaxReasonableAllocation)
        return nullptr;
    return __wrap_malloc(size == 0 ? 1 : size);
}

[[nodiscard]] void *allocate_aligned(std::size_t size, std::size_t alignment) noexcept
{
    alloc_trail_push(size);
    if (size > kMaxReasonableAllocation)
        return nullptr;
    void *address = nullptr;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if ((alignment & (alignment - 1)) != 0)
        return nullptr;
    return __wrap_posix_memalign(&address, alignment, size == 0 ? 1 : size) == 0 ? address : nullptr;
}

} // namespace

extern "C" __attribute__((noinline, visibility("hidden"))) bool
ps5ObserveOwnedAllocation(const void *address) noexcept
{
    __asm__ volatile("" : : "r"(address) : "memory");
    return address != nullptr;
}

/* --- Linker-wrapped libc allocators (see the --wrap flags in the link) ---- */

extern "C" void *__wrap_malloc(std::size_t size)
{
    if (size >= kPoolMinBlock)
    {
        alloc_trail_push(size);
        if (size <= kMaxReasonableAllocation)
        {
            if (void *address = pool_alloc(size))
                return address;
        }
        /* Pool refused (ceiling or kernel); the libc heap is the last resort
         * and historically cannot satisfy sizes like this, but try once. */
    }
    return __real_malloc(size);
}

extern "C" void __wrap_free(void *address)
{
    if (address == nullptr)
        return;
    PoolHeader *header = pool_header_of(address);
    if (header != nullptr)
    {
        pool_release(header);
        return;
    }
    __real_free(address);
}

extern "C" void *__wrap_calloc(std::size_t count, std::size_t size)
{
    std::size_t total = 0;
    if (__builtin_mul_overflow(count, size, &total))
        return nullptr;
    if (total < kPoolMinBlock)
        return __real_calloc(count, size);
    void *address = __wrap_malloc(total);
    if (address == nullptr)
        return nullptr;
    std::memset(address, 0, total);
    return address;
}

extern "C" void *__wrap_realloc(void *address, std::size_t size)
{
    if (address == nullptr)
        return __wrap_malloc(size);
    if (size == 0)
    {
        /* One consistent rule for both allocators: size zero releases. */
        __wrap_free(address);
        return nullptr;
    }
    PoolHeader *header = pool_header_of(address);
    if (header == nullptr)
        return __real_realloc(address, size);
    void *resized = __wrap_malloc(size);
    if (resized == nullptr)
        return nullptr;
    const std::size_t preserve = header->size < size ? header->size : size;
    std::memcpy(resized, address, preserve);
    __wrap_free(address);
    return resized;
}

extern "C" int __wrap_posix_memalign(void **address, std::size_t alignment, std::size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        return 22; /* EINVAL */
    /* Pool blocks are 16 KiB aligned, so any alignment up to 64 bytes is
     * satisfied by the standard pool pointer; larger alignments stay on libc. */
    if (size >= kPoolMinBlock && alignment <= kPoolHeader)
    {
        if (void *pooled = pool_alloc(size))
        {
            *address = pooled;
            return 0;
        }
    }
    return __real_posix_memalign(address, alignment, size);
}

/* --- Standard C++ allocation operators ----------------------------------- */

void *operator new(std::size_t size)
{
    if (void *address = allocate(size))
        return address;
    allocation_failure(size);
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
    if (void *address = allocate_aligned(size, static_cast<std::size_t>(alignment)))
        return address;
    allocation_failure(size);
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *address) noexcept
{
    __wrap_free(address);
}

void operator delete[](void *address) noexcept
{
    __wrap_free(address);
}

void operator delete(void *address, std::size_t) noexcept
{
    __wrap_free(address);
}

void operator delete[](void *address, std::size_t) noexcept
{
    __wrap_free(address);
}

void operator delete(void *address, std::align_val_t) noexcept
{
    __wrap_free(address);
}

void operator delete[](void *address, std::align_val_t) noexcept
{
    __wrap_free(address);
}

void operator delete(void *address, std::size_t, std::align_val_t) noexcept
{
    __wrap_free(address);
}

void operator delete[](void *address, std::size_t, std::align_val_t) noexcept
{
    __wrap_free(address);
}
