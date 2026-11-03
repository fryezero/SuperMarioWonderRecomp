// Pipeline warm-up and hand-over (nvn_pipeline_warm.h).
//
// The warm-up: at start, background threads rebuild the capture file's pipeline records in the order they were
// captured (the title screen's first, then the first course's) into a pipeline cache of their own, and keep every
// pipeline they build. The hand-over: a pipeline the replay's own caches miss is looked up by its record among those
// and the replay's earlier ones. Built: the replay takes it, no vkCreate at all; being built by a warm-up thread: the
// replay waits for it; else the replay claims it, so that no warm-up thread starts it, and creates it. On a cold iPhone
// the first version, which compiled into the replay's cache and destroyed what it built, stalled the boot behind its
// compiles and gained nothing until the next start: the course still spent 3.8 s on 208 pipelines it had just built.
// Different PipelineKeys often come to the same create info (the replay made three pipelines per distinct one): those
// share one pipeline the same way.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__ANDROID__)
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "nvn_pipeline.h"
#include "nvn_pipeline_warm.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// A create info by its record's 64-bit hash, the identity the capture file gives records: who builds it, and its
// pipeline, kept for the process. Free: nobody has built it (or building it failed); Building: a warm-up thread is;
// Claimed: the replay is.
struct Entry {
    enum State : uint8_t { Free, Building, Claimed, Built };
    State state = Free;
    bool warm = false;   // built by the warm-up
    bool taken = false;  // a warm-up pipeline the replay has used
    VkPipeline pipeline = VK_NULL_HANDLE;
#if defined(__APPLE__)
    pthread_t builder{};  // the warm-up thread building it
#endif
};

// The entries (under `lock`), and how the replay's misses were served: by a warm-up pipeline, ready (handed) or waited
// for, by another key's pipeline of the same create info, or by a new one.
struct Table {
    std::mutex lock;
    std::condition_variable done;  // an entry left Building or Claimed
    std::unordered_map<uint64_t, Entry> entries;  // never erased: references to them stay valid
    uint64_t misses = 0, handed = 0, waited = 0, shared = 0, created = 0, unrecorded = 0, wait_ns = 0;
    uint64_t kept = 0, taken = 0;  // pipelines the warm-up built, and how many of those the replay has used
    uint64_t logged = ~0ull;
};

Table& table() {
    static Table& t = *new Table;  // never destroyed: the warm-up's threads outlive static destructors
    return t;
}

// While the replay waits for a pipeline a warm-up thread is building, that thread runs at the replay's QoS: at utility
// QoS its compile waits for an efficiency core, and a condition variable passes no priority on.
struct Boost {
#if defined(__APPLE__)
    pthread_override_t o = nullptr;
    explicit Boost(const Entry& e) {
        if (e.state == Entry::Building) o = pthread_override_qos_class_start_np(e.builder, qos_class_self(), 0);
    }
    ~Boost() {
        if (o) pthread_override_qos_class_end_np(o);
    }
#else
    explicit Boost(const Entry&) {}
#endif
};

template <class CI>
VkPipeline create(const CI& ci) {
    VkPipeline p = VK_NULL_HANDLE;
    return pipeline_cache_create(ci, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

template <class CI>
VkPipeline serve(uint32_t kind, const CI& ci) {
    Table& t = table();
    std::vector<uint8_t> body;
    uint64_t key = 0;
    if (!capture_record(ci, body, key)) {  // no record, so neither handed over nor shared
        VkPipeline p = create(ci);
        std::lock_guard lk(t.lock);
        t.misses++, t.created++, t.unrecorded++;
        return p;
    }
    std::unique_lock lk(t.lock);
    t.misses++;
    Entry& e = t.entries[key];
    bool waited = false;
    uint64_t t0 = 0;
    while (e.state == Entry::Building || e.state == Entry::Claimed) {
        if (!waited) waited = true, t0 = plat::monotonic_ns();
        Boost boost(e);
        t.done.wait(lk);
    }
    if (waited) t.wait_ns += plat::monotonic_ns() - t0;
    if (e.state == Entry::Built) {
        (!e.warm ? t.shared : waited ? t.waited : t.handed)++;
        if (e.warm && !e.taken) e.taken = true, t.taken++;
        return e.pipeline;
    }
    e.state = Entry::Claimed;
    lk.unlock();
    VkPipeline p = create(ci);
    if (p) capture_add(kind, key, body);  // for the next start's warm-up
    lk.lock();
    e.state = p ? Entry::Built : Entry::Free, e.pipeline = p, e.warm = false;
    t.created++;
    t.done.notify_all();
    return p;
}

// ---- the warm-up ----------------------------------------------------------------------------------------------

struct Warm {
    VkPipelineCache cache = VK_NULL_HANDLE;  // its own: a fork of the pipeline cache
    std::vector<Record> pipelines;
    std::atomic<size_t> next{0};
    std::atomic<uint32_t> running{0}, built{0}, skipped{0}, failed{0}, unsupported{0};
    uint64_t start_ns = 0;
};

bool format_has(VkFormat f, VkFormatFeatureFlags want, bool buffer) {
    if (f == VK_FORMAT_UNDEFINED) return true;
    VkFormatProperties p{};
    p_vkGetPhysicalDeviceFormatProperties(vk_physical(), f, &p);
    return ((buffer ? p.bufferFeatures : p.optimalTilingFeatures) & want) == want;
}

// A record made on another device may need what this one lacks (a format, depth clip control, depth clamp): the
// replay never asks for that pipeline here, so it is skipped rather than handed to the driver.
bool supported(const VkGraphicsPipelineCreateInfo& ci) {
    bool ok = true;
    auto chain = [&](const void* next) {
        for (auto* s = static_cast<const VkBaseInStructure*>(next); s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                const auto& r = *reinterpret_cast<const VkPipelineRenderingCreateInfo*>(s);
                for (uint32_t k = 0; k < r.colorAttachmentCount; k++)
                    ok = ok && format_has(r.pColorAttachmentFormats[k], VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, false);
                ok = ok && format_has(r.depthAttachmentFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, false) &&
                     format_has(r.stencilAttachmentFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, false);
            } else if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT) {
                ok = ok && vk_depth_clip_control();
            }
    };
    chain(ci.pNext);
    if (ci.pViewportState) chain(ci.pViewportState->pNext);
    if (const auto* v = ci.pVertexInputState)
        for (uint32_t k = 0; k < v->vertexAttributeDescriptionCount; k++)
            ok = ok && format_has(v->pVertexAttributeDescriptions[k].format, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, true);
    if (ci.pRasterizationState && ci.pRasterizationState->depthClampEnable) ok = ok && vk_features().depthClamp;
    return ok;
}

VkPipeline build(Warm& w, const Record& r, size_t index) {
    Arena arena;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult made = VK_ERROR_INITIALIZATION_FAILED;
    bool read = false;
    if (r.kind == kRecordGraphics) {
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        read = read_record(r, arena, ci);
        if (read && !supported(ci)) {
            w.unsupported++;
            return VK_NULL_HANDLE;
        }
        if (read) made = p_vkCreateGraphicsPipelines(vk_device(), w.cache, 1, &ci, nullptr, &p);
    } else {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        read = read_record(r, arena, ci);
        if (read) made = p_vkCreateComputePipelines(vk_device(), w.cache, 1, &ci, nullptr, &p);
    }
    if (made == VK_SUCCESS) {
        w.built++;
        return p;
    }
    if (w.failed++ < 5)
        log("nvn/vk: pipeline warm-up: record %zu (%s) failed: %s (VkResult %d)", index,
            r.kind == kRecordGraphics ? "graphics" : "compute",
            read ? "refused" : "damaged, or a module or layout it names could not be made", int(made));
    return VK_NULL_HANDLE;
}

void finish(Warm* w) {
    records_done();
    uint64_t taken = 0;
    {
        Table& t = table();
        std::lock_guard lk(t.lock);
        taken = t.taken;
    }
    log("nvn/vk: pipeline warm-up: %u built and kept, %u left to the replay (it got there first), %u failed, %u not "
        "supported here, %.0f ms; the replay has taken %llu of them so far",
        w->built.load(), w->skipped.load(), w->failed.load(), w->unsupported.load(),
        double(plat::monotonic_ns() - w->start_ns) / 1e6, (unsigned long long)taken);
    pipeline_cache_join(w->cache);
    delete w;
}

// The warm-up's thread. On iOS one thread at user-initiated QoS, still below the game's frame threads: the Metal
// compiler service works at the QoS of whoever asks, and at utility the phone took 110 ms a pipeline (53 s for 483 on a
// cold cache) against 42 ms at the replay's own priority, so the game reached pipelines the warm-up had not built yet
// and waited for them. Elsewhere, utility like the capture writer: many threads, which must leave the cores to the game.
void warm_thread_priority() {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#else
    lower_priority();
#endif
}

void warm_thread(void* arg) {
    warm_thread_priority();
    plat::set_thread_name("nvn warm");  // 15 characters at most (Android)
    auto* w = static_cast<Warm*>(arg);
    Table& t = table();
    for (size_t k; (k = w->next.fetch_add(1)) < w->pipelines.size();) {
        const Record& r = w->pipelines[k];
        Entry* e = nullptr;
        {
            std::lock_guard lk(t.lock);
            e = &t.entries[r.key];
            if (e->state != Entry::Free) {  // the replay has it, or is creating it
                w->skipped++;
                continue;
            }
            e->state = Entry::Building;
#if defined(__APPLE__)
            e->builder = pthread_self();
#endif
        }
        VkPipeline p = build(*w, r, k);
        std::lock_guard lk(t.lock);
        e->state = p ? Entry::Built : Entry::Free;  // a failed one is left to the replay, should it ask for it
        e->pipeline = p, e->warm = p != VK_NULL_HANDLE;
        t.kept += p != VK_NULL_HANDLE;
        t.done.notify_all();
    }
    if (w->running.fetch_sub(1) == 1) finish(w);
}

// NX_PIPELINE_WARM=0: no warm-up; =<n>: n threads. By default one on iOS: on the iPhone 18 Pro Max with a cold Metal
// cache, 4 threads built 483 pipelines in 46 s (96 ms each) where one thread compiles one in 42 ms, its Metal compiles
// contending rather than running in parallel. Elsewhere every hardware thread but two (the game's main thread and the
// replay keep theirs), at least one.
uint32_t warm_threads() {
    if (const char* e = std::getenv("NX_PIPELINE_WARM"); e && *e) {
        char* end = nullptr;
        unsigned long n = std::strtoul(e, &end, 10);
        if (end != e) return uint32_t(std::min(n, 64ul));
    }
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return 1;
#else
    unsigned hw = std::thread::hardware_concurrency();
    return hw > 3 ? hw - 2 : 1;
#endif
}

}  // namespace

// Below the game's threads (USER_INTERACTIVE on Darwin, platform_darwin.cpp): the capture and the warm-up must not
// take the performance cores from the game's main thread and the replay, so they run at utility QoS, on the
// efficiency cores whenever those threads want the others.
void lower_priority() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__ANDROID__)
    setpriority(PRIO_PROCESS, gettid(), 10);
#endif
}

bool warm_start(std::vector<Record> pipelines) {
    uint32_t threads = uint32_t(std::min<size_t>(warm_threads(), pipelines.size()));
    if (!threads) return false;
    auto* w = new Warm;
    w->pipelines = std::move(pipelines);
    w->cache = pipeline_cache_fork();
    w->running = threads;
    w->start_ns = plat::monotonic_ns();
    log("nvn/vk: pipeline warm-up: %zu pipelines on %u thread%s", w->pipelines.size(), threads, threads > 1 ? "s" : "");
    for (uint32_t k = 0; k < threads; k++) plat::start_thread(warm_thread, w, 8ull << 20);  // MoltenVK compiles
    return true;
}

VkPipeline replay_pipeline(const VkGraphicsPipelineCreateInfo& ci) { return serve(kRecordGraphics, ci); }
VkPipeline replay_pipeline(const VkComputePipelineCreateInfo& ci) { return serve(kRecordCompute, ci); }

void handover_log() {
    Table& t = table();
    std::lock_guard lk(t.lock);
    if (t.misses == t.logged) return;
    t.logged = t.misses;
    log("nvn/vk: pipeline hand-over: %llu replay misses this run: %llu served by the warm-up's pipelines (%llu "
        "distinct, %llu waited for), %llu by one the replay created for another key, %llu created (%llu not "
        "recordable), %.1f ms waiting; the warm-up keeps %llu pipelines",
        (unsigned long long)t.misses, (unsigned long long)(t.handed + t.waited), (unsigned long long)t.taken,
        (unsigned long long)t.waited, (unsigned long long)t.shared, (unsigned long long)t.created,
        (unsigned long long)t.unrecorded, double(t.wait_ns) / 1e6, (unsigned long long)t.kept);
}

}  // namespace nx::nvn
