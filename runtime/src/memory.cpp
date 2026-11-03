// Guest address space on the host: reserved ranges, commit on demand, and a Horizon-style memory map
// for svcQueryMemory.
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif
#endif

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

GuestLayout g_layout;

namespace {

std::mutex g_vm_lock;
std::map<uint64_t, VmInfo> g_vm;  // start -> block (non-overlapping; gaps are free)
uint64_t g_kernel_next = 0;

void reserve(uint64_t addr, uint64_t size, const char* what) {
    if (!plat::reserve(addr, size))
        fatal("cannot reserve %s at %llx (+%llx): the range is in use", what, (unsigned long long)addr,
              (unsigned long long)size);
}

constexpr uint64_t kGiB = 0x40000000ull, kMiB = 0x100000ull;

#if defined(__APPLE__) && !TARGET_OS_IPHONE
// macOS puts a 24 GiB heap range at a random place in 0x70..0x80_0000_0000 at every launch, now and then over the
// modules' fixed place (NX_BASE is compiled into the generated code). The process then starts itself again, a few
// times at most, and gets another random range. iOS has no such range in the window.
void restart_for_another_layout() {
    const char* tries = std::getenv("NX_LAYOUT_RETRY");
    int n = tries ? std::atoi(tries) : 0;
    if (n >= 4) return;
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return;
    setenv("NX_LAYOUT_RETRY", std::to_string(n + 1).c_str(), 1);
    log("memory: the modules' place %llx is taken by the system this launch: starting again",
        (unsigned long long)kModuleBase);
    execv(path, *_NSGetArgv());  // only returns if it failed
}
#endif

#if defined(__APPLE__)
// Apple: the regions go below the modules, top down, into what the host leaves free in [kWindowBase, kModuleBase).
// Each region gets its preferred size where a free run holds it, else as much as the largest run has, down to its
// minimum: what Wonder needs (svcSetHeapSize(0x5BE00000), a 1750 MiB VAMM budget in the alias region) and a stack
// region big enough for nn::os's random placement of shared and transfer memory (svc_process.cpp, GetInfo 12/13).
constexpr uint64_t kWindowBase = 0x7000000000ull;
constexpr uint64_t kChunk = 256 * kMiB;  // probe granularity

struct Run {
    uint64_t base, size;
};

// Free runs of [lo, hi): reserve each chunk and let it go again. Ranges the kernel keeps for itself without listing
// them (macOS's heap range) only show up this way.
std::vector<Run> free_runs(uint64_t lo, uint64_t hi) {
    std::vector<Run> runs;
    for (uint64_t a = lo; a < hi; a += kChunk) {
        if (!plat::reserve(a, kChunk)) continue;
        plat::release(a, kChunk);
        if (!runs.empty() && runs.back().base + runs.back().size == a)
            runs.back().size += kChunk;
        else
            runs.push_back({a, kChunk});
    }
    return runs;
}

void place_regions() {
    std::vector<Run> runs = free_runs(kWindowBase, kModuleBase);
    struct Want {
        const char* name;
        uint64_t pref, min;
        uint64_t *base, *size;
    } wants[] = {
        {"alias", 16 * kGiB, 2 * kGiB, &g_layout.alias_base, &g_layout.alias_size},
        {"heap", 6 * kGiB, 2 * kGiB, &g_layout.heap_base, &g_layout.heap_max},
        {"stack", 4 * kGiB, 2 * kGiB, &g_layout.stack_base, &g_layout.stack_size},
        {"kernel", 4 * kGiB, 256 * kMiB, &g_layout.kernel_base, &g_layout.kernel_size},
        {"thunks", kMiB, kMiB, &g_layout.thunk_base, &g_layout.thunk_size},
    };
    for (Want& w : wants) {
        Run* pick = nullptr;
        for (Run& r : runs)  // the highest run that holds the preferred size (nearest the modules)
            if (r.size >= w.pref && (!pick || r.base > pick->base)) pick = &r;
        if (!pick)
            for (Run& r : runs)  // else the largest one that holds the minimum
                if (r.size >= w.min && (!pick || r.size > pick->size)) pick = &r;
        if (!pick) {
            std::string have;
            for (const Run& r : runs) {
                char b[64];
                snprintf(b, sizeof b, " %llx+%llx", (unsigned long long)r.base, (unsigned long long)r.size);
                have += b;
            }
            fatal("no room for the guest's %s region (at least %llu MiB) below %llx; free:%s%s", w.name,
                  (unsigned long long)(w.min / kMiB), (unsigned long long)kModuleBase, have.empty() ? " none" : have.c_str(),
#if TARGET_OS_IPHONE
                  ". The app needs the com.apple.developer.kernel.increased-memory-limit entitlement");
#else
                  "");
#endif
        }
        uint64_t size = std::min(w.pref, pick->size);
        *w.size = size;
        *w.base = pick->base + pick->size - size;  // from the top of the run
        pick->size -= size;
    }
}
#endif

}  // namespace

// Safe to call more than once: iOS calls it first thing in main(), before UIKit and Metal take address space in the
// window the guest needs, and nx::boot calls it again.
void memory_init() {
    static bool done = false;
    if (done) return;
    done = true;
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    if (!plat::reserve(kModuleBase, kModuleSize)) restart_for_another_layout();
    else plat::release(kModuleBase, kModuleSize);
#endif
    reserve(kModuleBase, kModuleSize, "modules");
#if defined(__APPLE__)
    place_regions();
#else
    g_layout.space_base = 0x0008000000ull;
    g_layout.space_size = 0x7FF8000000ull;  // 39-bit, like an application on Horizon
    g_layout.heap_base = 0x5000000000ull, g_layout.heap_max = 6 * kGiB;
#if defined(__ANDROID__)
    // Android maps the zygote's executable (app_process64, position independent) at a random address from about
    // 0x55_5555_0000 to 0x65_5555_0000 at every boot, inside 0x58..0x68: a Galaxy S23 refused the alias reservation
    // ("the range is in use", app_process64 at 0x5f_0972_b000). Below the modules nothing is mapped (ART keeps to
    // the low 4 GiB), so the alias region goes there.
    g_layout.alias_base = 0x2000000000ull;
#else
    g_layout.alias_base = 0x5800000000ull;
#endif
    g_layout.alias_size = 64 * kGiB;
    // Kept below 0x70_0000_0000: Android (39-bit address space) maps the process's own libraries and heap up there.
    g_layout.stack_base = 0x6800000000ull, g_layout.stack_size = 4 * kGiB;
    g_layout.kernel_base = 0x6A00000000ull, g_layout.kernel_size = 4 * kGiB;
    g_layout.thunk_base = 0x6F00000000ull, g_layout.thunk_size = kMiB;
#endif
    GuestLayout& L = g_layout;
    reserve(L.heap_base, L.heap_max, "heap");
    reserve(L.alias_base, L.alias_size, "alias");
    reserve(L.stack_base, L.stack_size, "stack");
    reserve(L.kernel_base, L.kernel_size, "kernel");
    reserve(L.thunk_base, L.thunk_size, "thunks");
#if defined(__APPLE__)
    // The ASLR region spans the guest's regions and nothing more: nn::os places shared and transfer memory at random
    // addresses in it (up to 512 tries) and only the stack region is free there, so a narrow span makes tries land.
    uint64_t lo = kModuleBase, hi = kModuleBase + kModuleSize;
    for (uint64_t b : {L.heap_base, L.alias_base, L.stack_base, L.kernel_base, L.thunk_base}) lo = std::min(lo, b);
    L.space_base = lo, L.space_size = hi - lo;
#endif
    g_kernel_next = L.kernel_base;
    log("memory: modules %llx, heap %llx+%llx, alias %llx+%llx, stack %llx+%llx, kernel %llx+%llx, thunks %llx, "
        "address space %llx+%llx",
        (unsigned long long)kModuleBase, (unsigned long long)L.heap_base, (unsigned long long)L.heap_max,
        (unsigned long long)L.alias_base, (unsigned long long)L.alias_size, (unsigned long long)L.stack_base,
        (unsigned long long)L.stack_size, (unsigned long long)L.kernel_base, (unsigned long long)L.kernel_size,
        (unsigned long long)L.thunk_base, (unsigned long long)L.space_base, (unsigned long long)L.space_size);
    // Only the alias and stack regions are free for the guest to pick addresses in (nn::os maps shared memory,
    // transfer memory and thread stacks at random free addresses); everything else in the address space, including
    // host memory between our regions, is reported inaccessible.
    vm_set(L.space_base, L.space_size, kMemInaccessible, 0);
    vm_set(L.alias_base, L.alias_size, kMemFree, 0);
    vm_set(L.stack_base, L.stack_size, kMemFree, 0);
}

bool commit(uint64_t addr, uint64_t size) {
    return plat::commit(addr, size);
}

void decommit(uint64_t addr, uint64_t size) {
    plat::decommit(addr, size);
}

uint64_t kernel_alloc(uint64_t size, uint64_t align) {
    std::lock_guard lk(g_vm_lock);
    uint64_t a = (g_kernel_next + align - 1) & ~(align - 1);
    size = (size + 0xFFF) & ~0xFFFull;
    if (a + size > g_layout.kernel_base + g_layout.kernel_size) fatal("kernel region exhausted");
    g_kernel_next = a + size;
    if (!commit(a, size)) fatal("kernel_alloc commit failed");
    return a;
}

void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm) {
    std::lock_guard lk(g_vm_lock);
    uint64_t end = addr + size;
    // Cut overlapping blocks.
    auto it = g_vm.lower_bound(addr);
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (p->second.addr + p->second.size > addr) it = p;
    }
    while (it != g_vm.end() && it->second.addr < end) {
        VmInfo b = it->second;
        it = g_vm.erase(it);
        if (b.addr < addr) g_vm[b.addr] = {b.addr, addr - b.addr, b.state, b.perm};
        if (b.addr + b.size > end) g_vm[end] = {end, b.addr + b.size - end, b.state, b.perm};
    }
    if (state != kMemFree) g_vm[addr] = {addr, size, state, perm};
}

VmInfo vm_query(uint64_t addr) {
    std::lock_guard lk(g_vm_lock);
    auto it = g_vm.upper_bound(addr);
    uint64_t gap_end = it == g_vm.end() ? g_layout.space_base + g_layout.space_size : it->first;
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (addr < p->second.addr + p->second.size) return p->second;
        uint64_t gap_start = p->second.addr + p->second.size;
        return {gap_start, gap_end - gap_start, kMemFree, 0};
    }
    // Below the address space nothing is the guest's: never offer it as free.
    if (addr < g_layout.space_base) return {0, g_layout.space_base, kMemInaccessible, 0};
    return {g_layout.space_base, gap_end - g_layout.space_base, kMemFree, 0};
}

}  // namespace nx
