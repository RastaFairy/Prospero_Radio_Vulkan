// Host-side unit test for the ProsperoRadio allocation pool.
// Mocks the PS5 Direct-Memory kernel APIs over a 700 MB host arena and links
// the real overlay/src/app_cpp_runtime.cpp, then stresses malloc/calloc/
// realloc/free exactly like the target would exercise them through --wrap.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

static std::vector<std::uint8_t> g_arena;
static std::size_t g_arena_used = 0;

extern "C"
{
    std::size_t sceKernelGetDirectMemorySize() { return 0x90000000ull; }

    int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t length,
                                      std::size_t alignment, int, std::int64_t *physical)
    {
        g_arena_used = (g_arena_used + alignment - 1) & ~(alignment - 1);
        if (g_arena_used + length > g_arena.size())
            return -1;
        *physical = static_cast<std::int64_t>(g_arena_used);
        g_arena_used += length;
        return 0;
    }

    int sceKernelMapDirectMemory(void **address, std::size_t, int, int,
                                 std::int64_t physical, std::size_t)
    {
        // The real kernel returns 16 KiB aligned mappings; mirror that here by
        // carving the aligned view of the arena instead of the raw buffer.
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_arena.data());
        const std::uintptr_t aligned = (base + 0x3FFF) & ~static_cast<std::uintptr_t>(0x3FFF);
        *address = reinterpret_cast<void *>(aligned + static_cast<std::size_t>(physical));
        return 0;
    }

    void *__real_malloc(std::size_t size) { return malloc(size); }
    void __real_free(void *address) { free(address); }
    void *__real_realloc(void *address, std::size_t size) { return realloc(address, size); }
    void *__real_calloc(std::size_t count, std::size_t size) { return calloc(count, size); }
    int __real_posix_memalign(void **address, std::size_t alignment, std::size_t size)
    {
        return posix_memalign(address, alignment, size);
    }
}

#include "../overlay/src/app_cpp_runtime.cpp"

static std::size_t g_failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main()
{
    g_arena.assign(700ull * 1024ull * 1024ull, 0);

    // 1. large calloc must come from the pool and be zeroed
    auto *table = static_cast<std::uint8_t *>(__wrap_calloc(1, 1221859));
    check(table != nullptr, "large calloc succeeds");
    bool all_zero = true;
    for (std::size_t i = 0; i < 1221859; ++i)
        all_zero = all_zero && table[i] == 0;
    check(all_zero, "large calloc zeroed");
    check((reinterpret_cast<std::uintptr_t>(table) & 15) == 0, "large calloc 16-aligned");

    // 2. large malloc gets a distinct, non-overlapping block
    auto *big = static_cast<std::uint8_t *>(__wrap_malloc(2213681));
    check(big != nullptr, "large malloc succeeds");
    std::memset(big, 0xAB, 2213681);
    check(std::count(table, table + 1221859, 0xAB) == 0, "pool blocks do not overlap");

    // 3. realloc preserves contents (pooled -> pooled)
    auto *grown = static_cast<std::uint8_t *>(__wrap_realloc(big, 4213681));
    check(grown != nullptr, "realloc grows");
    bool kept = true;
    for (std::size_t i = 0; i < 2213681; ++i)
        kept = kept && grown[i] == 0xAB;
    check(kept, "realloc preserved contents");

    // 4. free returns blocks to the cache; a same-size allocation must reuse
    //    one of them instead of mapping more memory
    __wrap_free(table);
    __wrap_free(grown);
    const std::size_t used_before = g_arena_used;
    auto *reuse = static_cast<std::uint8_t *>(__wrap_malloc(1221859));
    check(reuse != nullptr, "reuse allocation succeeds");
    check(g_arena_used == used_before, "reuse does not map new memory");

    // 5. small allocations keep using the libc heap
    auto *small = static_cast<char *>(__wrap_malloc(128));
    check(small != nullptr, "small malloc succeeds");
    __wrap_free(small);

    // 6. multithreaded stress: mixed sizes crossing the threshold
    std::atomic<unsigned> seed{12345};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&seed]() {
            std::mt19937 rng(seed.fetch_add(7919));
            std::vector<std::pair<void *, std::size_t>> live;
            for (int op = 0; op < 20000; ++op)
            {
                const bool big = (rng() % 4) == 0;
                const std::size_t size = big ? (256 * 1024 + rng() % (1024 * 1024))
                                             : (rng() % 4096);
                const int kind = static_cast<int>(rng() % 3);
                if (kind == 0 || live.empty())
                {
                    void *p = (rng() & 1) ? __wrap_malloc(size) : __wrap_calloc(1, size);
                    if (p != nullptr)
                        live.emplace_back(p, size);
                }
                else if (kind == 1)
                {
                    std::size_t idx = rng() % live.size();
                    const std::size_t resize = size == 0 ? 1 : size;
                    void *p = __wrap_realloc(live[idx].first, resize);
                    if (p != nullptr)
                    {
                        live[idx] = {p, resize};
                    }
                    else if (resize > 0)
                    {
                        /* realloc failed without touching the original; drop it
                         * so the harness never frees it twice. */
                        live[idx] = live.back();
                        live.pop_back();
                    }
                }
                else
                {
                    std::size_t idx = rng() % live.size();
                    __wrap_free(live[idx].first);
                    live[idx] = live.back();
                    live.pop_back();
                }
            }
            for (auto &entry : live)
                __wrap_free(entry.first);
        });
    }
    for (auto &thread : threads)
        thread.join();
    check(g_failures == 0, "stress produced no failures");

    // 7. operator new/delete wiring (>= threshold goes to the pool)
    auto *via_new = new std::uint8_t[1221859];
    check(via_new != nullptr, "new succeeds for pool-sized request");
    std::memset(via_new, 0xCD, 1221859);
    delete[] via_new;

    if (g_failures == 0)
    {
        std::printf("ALL POOL TESTS PASSED (arena used: %zu bytes)\n", g_arena_used);
        return 0;
    }
    return 1;
}
