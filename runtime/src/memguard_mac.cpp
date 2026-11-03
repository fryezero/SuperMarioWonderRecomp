// Memory guard (macOS, iOS): the process's physical footprint (what Activity Monitor shows, GPU memory included on
// Apple silicon) is checked every 250 ms; past the limit the process ends at once, so a leak can never push the machine
// into swap until it freezes (it did: 85 GB, texture images never freed). The footprint is logged every 30 s so growth
// shows before it matters.
// macOS: the limit is NX_MEM_LIMIT_GB (default 8, half of a 16 GB Mac).
// iOS: the limit is what jetsam allows the app, its footprint plus os_proc_available_memory() at launch (about 6.1 GB
// on a 12 GB iPhone with the increased-memory-limit entitlement). The kernel ends the app there without a word in
// nx.log, so the log lines also give the memory still available, a warning is written once when that falls below
// 400 MB, and iOS's memory warnings are logged the same way (platform_ios.mm calls log_memory).
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <nx/runtime.h>

#include "platform.h"
#include "platform_darwin.h"

namespace nx::plat {

namespace {

constexpr double kGiB = double(1ull << 30);

uint64_t footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

std::atomic<uint64_t> g_peak{0};

}  // namespace

#if TARGET_OS_IPHONE
void log_memory(const char* why) {
    uint64_t f = footprint();
    log("memory%s%s: %.2f GB (peak %.2f GB), %.2f GB available, thermal %s", why ? ", " : "", why ? why : "",
        double(f) / kGiB, double(std::max(f, g_peak.load())) / kGiB, double(os_proc_available_memory()) / kGiB,
        thermal_state());
}
#endif

void start_memory_guard() {
#if TARGET_OS_IPHONE
    const uint64_t available = os_proc_available_memory();
    if (!available) {  // os/proc.h: 0 when the process is not an app, which has no limit to watch
        log("memory: iOS sets no limit (not run as an app): no memory guard");
        return;
    }
    const uint64_t limit = footprint() + available;
    const double gb = double(limit) / kGiB;
    const char* source = "iOS's at launch";
    log("memory: iOS allows this app about %.2f GB", gb);
#else
    const char* e = std::getenv("NX_MEM_LIMIT_GB");
    double gb = e ? std::atof(e) : 8.0;
    if (gb <= 0) gb = 8.0;
    const uint64_t limit = uint64_t(gb * kGiB);
    const char* source = "NX_MEM_LIMIT_GB";
#endif
    std::thread([limit, gb, source] {
        uint64_t ticks = 0;
#if TARGET_OS_IPHONE
        bool warned = false;
#endif
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            uint64_t f = footprint();
            if (f > g_peak) g_peak = f;
#if TARGET_OS_IPHONE
            if (++ticks % 120 == 0) log_memory(nullptr);
            // In the background iOS allows less, but the game holds there: only a shortfall in play is worth a warning.
            if (!warned && os_proc_available_memory() < (400ull << 20) && !window_minimized()) {
                warned = true;
                log("memory: under 400 MB left before iOS ends the app (%.2f GB in use)", double(f) / kGiB);
            }
            // iOS may raise its limit later (os/proc.h): past the launch one only counts while nothing is left.
            const bool over = f > limit && os_proc_available_memory() == 0;
#else
            if (++ticks % 120 == 0)
                log("memory: %.2f GB (peak %.2f GB, limit %.1f GB)", double(f) / kGiB, double(g_peak) / kGiB, gb);
            const bool over = f > limit;
#endif
            if (over) {
                char msg[160];
                std::snprintf(msg, sizeof msg, "memory: %.2f GB is over the %.1f GB limit (%s): stopping",
                              double(f) / kGiB, gb, source);
                log("%s", msg);
                std::fprintf(stderr, "%s\n", msg);
                _exit(3);
            }
        }
    }).detach();
}

uint64_t memguard_overlap_ns(uint64_t, uint64_t) { return 0; }

}  // namespace nx::plat
