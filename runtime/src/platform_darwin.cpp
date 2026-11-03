// Darwin host parts shared by macOS (platform_mac.cpp) and iOS (platform_ios.mm): memory (mmap at fixed addresses),
// pthreads, time, files, stderr logging, the signal crash handler and the game thread. What differs between the two
// (window, input, audio, the Vulkan surface, data location, settings, main) stays in the front ends.
#include <dlfcn.h>
#include <mach/thread_switch.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "platform_darwin.h"

namespace nx::plat {

// ---- memory ---------------------------------------------------------------------------------------------------

// No MAP_FIXED_NOREPLACE on Darwin: ask for the address as a hint and give it back if the kernel chose another.
bool reserve(uint64_t addr, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (uint64_t(p) != addr) {
        munmap(p, size);
        return false;
    }
    return true;
}

void release(uint64_t addr, uint64_t size) {
    munmap(reinterpret_cast<void*>(addr), size);
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize()) - 1;  // 16 KiB on Apple silicon
    uint64_t a = addr & ~page, e = (addr + size + page) & ~page;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

// The guest frees 4 KiB pages; Apple silicon pages are 16 KiB, and mprotect would take the neighbours of a partly
// freed page with it (SIGBUS in Wonder when nn::os's VAMM unmapped single pages). Only whole host pages are
// released; the freed part of a shared one is zeroed, which is what the guest would see if it maps it again.
// Whole pages are replaced by a fresh no-access mapping rather than madvise(MADV_FREE)'d: MADV_FREE pages stay in the
// task's phys_footprint until the kernel happens to reclaim them, and iOS jetsam kills the app on that footprint. The
// new mapping drops them at once and reads back as zeros when committed again.
void decommit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize());
    uint64_t lo = (addr + page - 1) & ~(page - 1), hi = (addr + size) & ~(page - 1);
    if (lo >= hi) {
        std::memset(reinterpret_cast<void*>(addr), 0, size);
        return;
    }
    if (addr < lo) std::memset(reinterpret_cast<void*>(addr), 0, lo - addr);
    if (hi < addr + size) std::memset(reinterpret_cast<void*>(hi), 0, addr + size - hi);
    mmap(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    // The game's threads (render, jobs, audio) are latency critical: default QoS lets the scheduler run them on the
    // efficiency cores and stretch their sleeps and wakeups.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (stack_size + 0x3FFF) & ~size_t(0x3FFF));
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) { _exit(code); }

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    nanosleep(&ts, nullptr);
}

// macOS sched_yield drops the caller to priority 0 for a while: measured 20-180 us per call on the M1, and Wonder's
// job system yields after every job item and in its barriers, so the main thread spent ~60% of its frame there and
// the game's governor fell to 30 fps (docs/nvn/DEBUG.md #44). thread_switch with no hint (2-4 us) gives the core to
// another ready thread without that penalty. NX_YIELD=sched: the old call.
void yield() {
    static const bool sched = [] {
        const char* e = std::getenv("NX_YIELD");
        return e && std::strcmp(e, "sched") == 0;
    }();
    if (sched)
        sched_yield();
    else
        thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
}
void set_thread_name(const char* name) { pthread_setname_np(name); }  // Darwin: the calling thread only

int64_t current_thread_id() {
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return int64_t(tid);
}

bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }  // use Instruments

uint64_t monotonic_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }

uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    if (size > uint64_t(SSIZE_MAX)) return 0;  // the game passes -1 as "everything": fread read nothing
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

// ---- crash handler and the game thread ------------------------------------------------------------------------

namespace {

void crash_handler(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    const auto* m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    Dl_info d{};
    uint64_t pc = m->__ss.__pc;
    dladdr(reinterpret_cast<void*>(pc), &d);
    nx::log("CRASH: signal %d at %s+%llx, data address %llx", sig, d.dli_fname ? d.dli_fname : "?",
            (unsigned long long)(pc - uint64_t(d.dli_fbase)), (unsigned long long)uint64_t(info->si_addr));
    nx::CPU& c = nx::current_cpu();
    nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
    for (int k = 0; k < 32; k += 4)
        nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
                (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    nx::backtrace(c);
    signal(sig, SIG_DFL);
    raise(sig);
}

struct GameStart {
    void (*fn)(void*);
    void* arg;
};

void game_thread(void* p) {
    GameStart s = *static_cast<GameStart*>(p);
    delete static_cast<GameStart*>(p);
    try {
        s.fn(s.arg);
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

}  // namespace

void install_crash_handlers() {
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

void start_game_thread(void (*fn)(void*), void* arg) {
    start_thread(game_thread, new GameStart{fn, arg}, 64ull << 20);
}

}  // namespace nx::plat
