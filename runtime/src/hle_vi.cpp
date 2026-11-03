// nn::vi (display/layers) and nv:: (graphics driver setup) at the API level. The layer is the platform's window
// (Win32 window / Android surface); its handle is the "native window" the game hands to vkCreateViSurfaceNN, which
// the Vulkan bridge turns into the platform's surface.
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "vsync.h"

namespace nx {

namespace {

// Opaque pointers, never dereferenced by the game: the top of the thunk region, which is reserved and never mapped.
uint64_t fake_display() { return g_layout.thunk_base + g_layout.thunk_size - 0x1000; }
uint64_t fake_layer() { return g_layout.thunk_base + g_layout.thunk_size - 0x2000; }

void ok(CPU& c) { c.x[0] = 0; }

void OpenDefaultDisplay(CPU& c) {
    st<uint64_t>(c.x[0], fake_display());
    c.x[0] = 0;
}

void CreateLayer(CPU& c) {
    void* w = plat::create_window();
    log("vi: layer created -> native window %p", w);
    plat::release_window(w);
    st<uint64_t>(c.x[0], fake_layer());
    c.x[0] = 0;
}

void GetNativeWindow(CPU& c) {  // (void** out, Layer*)
    st<uint64_t>(c.x[0], uint64_t(plat::create_window()));
    c.x[0] = 0;
}

void nv_noop(CPU&) {}

// The display's vsync: a kernel event signalled every 1/60 s, by the display itself where the platform has a vsync
// source (iOS: CADisplayLink), else by a host thread's timer. The game gets it as an nn::os SystemEvent
// (AttachSystemEvent with our read/write handles, auto clear: each wait consumes one vsync).
std::shared_ptr<EventState> g_vsync = std::make_shared<EventState>();
std::atomic<uint64_t> g_display_vsync{0};  // display_vsync_ns()

void signal_vsync() {
    g_vsync->signaled = true;
    notify_all();
}

void vsync_thread() {
    plat::set_thread_name("vsync");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += kVsyncNs;
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        signal_vsync();
    }
}

// A display refresh (plat::vsync_source, on a platform thread). The game gets one vsync per 1/60 s whatever the panel
// runs at: a refresh less than 14.7 ms (16.7 - 2) after the last one it got is skipped, every other one at 120 Hz.
// The timestamp is the pacing's timeline (nvn_queue.cpp); one over a second from now is on another clock, and now
// stands in for it.
void display_tick(uint64_t vsync_ns) {
    static uint64_t last = 0;
    static bool logged = false;
    uint64_t now = plat::monotonic_ns();
    if (int64_t(now - vsync_ns) > 1'000'000'000 || int64_t(vsync_ns - now) > 1'000'000'000) {
        if (!logged)
            log("vi: display vsync %+.0f ms from now: not on the monotonic clock",
                (double(vsync_ns) - double(now)) / 1e6);
        logged = true;
        vsync_ns = now;
    }
    if (last && vsync_ns - last < kVsyncNs - 2'000'000) return;
    last = vsync_ns;
    g_display_vsync.store(vsync_ns, std::memory_order_release);
    signal_vsync();
}

void GetDisplayVsyncEvent(CPU& c) {  // (os::SystemEventType*, Display*)
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = g_vsync;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEventType; 1: AutoClear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    static std::once_flag once;
    std::call_once(once, [] {
        if (plat::vsync_source(display_tick)) {
            log("vi: vsync from the display");
        } else {
            log("vi: vsync from a 60 Hz timer");
            std::thread(vsync_thread).detach();
        }
    });
    c.x[0] = 0;
}

}  // namespace

uint64_t display_vsync_ns() { return g_display_vsync.load(std::memory_order_acquire); }

NX_HLE("_ZN2nn2vi10InitializeEv", ok);
NX_HLE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", OpenDefaultDisplay);
NX_HLE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", CreateLayer);
NX_HLE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", GetNativeWindow);
NX_HLE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", GetDisplayVsyncEvent);
NX_HLE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv18InitializeGraphicsEPvm", nv_noop);

}  // namespace nx
