// A VkPipelineCache kept on disk (<data root>/pipelines_<vendor>_<device>.bin), and the cost of what the replay
// creates on the fly. Pipelines are compiled the first time a draw needs them, inside the replay: the course's
// stutters when new objects come in (16-42 ms replays on the RTX 3050). The driver checks the cache header itself
// (vendor, device, driver build): a stale file is ignored. Saved every 600 presents when it grew, and when the
// warm-up (nvn_pipeline_warm.cpp) has merged in the cache it rebuilt the pipelines captured in earlier sessions into.
// NX_PIPELINE_CACHE=0: no cache (bisecting, or to measure first-run stutter).
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "nvn_pipeline_warm.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_tried = false;
size_t g_saved_size = 0;
std::string g_path;
std::mutex g_save_lock;  // the replay's periodic save and the warm-up's final one
// vkMergePipelineCaches' destination is externally synchronised: the warm-up's merge takes this exclusively, and
// everything else that uses g_cache (the replay's creates, the saves, the warm-up's fork) shared.
std::shared_mutex g_use_lock;
uint64_t g_pipelines = 0, g_pipeline_ns = 0, g_astc = 0, g_astc_ns = 0;

VkPipelineCache open_cache() {
    if (const char* e = std::getenv("NX_PIPELINE_CACHE"); e && e[0] == '0') return VK_NULL_HANDLE;
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines_%04x_%04x.bin", p.vendorID, p.deviceID);
    g_path = plat::data_root() + name;
    std::vector<uint8_t> data;
    if (FILE* f = plat::fopen_utf8(g_path, "rb")) {
        plat::fseek64(f, 0, SEEK_END);
        int64_t n = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_SET);
        if (n > 0) data.resize(size_t(n)), data.resize(std::fread(data.data(), 1, data.size(), f));
        std::fclose(f);
    }
    // Without VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT: the saves read it on other threads while the
    // replay creates pipelines with it, and the driver synchronises that.
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;  // a cache the driver refuses: start empty
        p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &cache);
    }
    g_saved_size = data.size();
    log("nvn/vk: pipeline cache %s (%zu bytes loaded)", g_path.c_str(), data.size());
    return cache;
}

// The cache's contents, under g_use_lock (shared); empty on failure, or when not larger than `skip` bytes.
std::vector<uint8_t> cache_data(size_t skip) {
    std::vector<uint8_t> data;
    size_t n = 0;
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, nullptr) != VK_SUCCESS || n <= skip) return data;
    data.resize(n);
    if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, data.data()) != VK_SUCCESS) n = 0;
    data.resize(n);
    return data;
}

// `wait` (pipeline_cache_save, the warm-up's final save): waits for a save in progress, and for its merge; the
// replay's periodic one skips its turn instead of stalling a frame behind either (a whole vkGetPipelineCacheData and
// file write on a utility thread).
void save(bool wait) {
    if (!g_cache) return;
    std::unique_lock lk(g_save_lock, std::defer_lock);
    std::shared_lock use(g_use_lock, std::defer_lock);
    if (wait) lk.lock(), use.lock();
    else if (!lk.try_lock() || !use.try_lock()) return;
    std::vector<uint8_t> data = cache_data(g_saved_size);
    use.unlock();
    if (data.empty()) return;
    std::string tmp = g_path + ".tmp";
    if (FILE* f = plat::fopen_utf8(tmp, "wb")) {
        bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = std::fclose(f) == 0 && ok;
        if (ok) std::remove(g_path.c_str());  // Windows' rename does not replace; a failed write keeps the old file
        if (ok && std::rename(tmp.c_str(), g_path.c_str()) == 0) g_saved_size = data.size();
    }
}

}  // namespace

VkPipelineCache pipeline_cache() {
    if (g_tried) return g_cache;
    g_tried = true;
    g_cache = open_cache();
    pipeline_capture_start(g_cache != VK_NULL_HANDLE);  // the capture file, and the warm-up that merges into g_cache
    return g_cache;
}

void pipeline_cache_save() { save(true); }

VkResult pipeline_cache_create(const VkGraphicsPipelineCreateInfo& ci, VkPipeline* p) {
    VkPipelineCache cache = pipeline_cache();
    std::shared_lock use(g_use_lock);
    return p_vkCreateGraphicsPipelines(vk_device(), cache, 1, &ci, nullptr, p);
}

VkResult pipeline_cache_create(const VkComputePipelineCreateInfo& ci, VkPipeline* p) {
    VkPipelineCache cache = pipeline_cache();
    std::shared_lock use(g_use_lock);
    return p_vkCreateComputePipelines(vk_device(), cache, 1, &ci, nullptr, p);
}

VkPipelineCache pipeline_cache_fork() {
    std::vector<uint8_t> data;
    if (g_cache) {
        std::shared_lock use(g_use_lock);
        data = cache_data(0);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    VkPipelineCache fork = VK_NULL_HANDLE;
    if (p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &fork) != VK_SUCCESS) fork = VK_NULL_HANDLE;
    log("nvn/vk: pipeline cache: the warm-up's own, seeded with %zu bytes%s", data.size(), fork ? "" : ": failed");
    return fork;
}

void pipeline_cache_join(VkPipelineCache fork) {
    if (!fork) return;
    if (g_cache) {
        const uint64_t t0 = plat::monotonic_ns();
        VkResult merged;
        {
            std::unique_lock use(g_use_lock);
            merged = p_vkMergePipelineCaches(vk_device(), g_cache, 1, &fork);
        }
        const double ms = double(plat::monotonic_ns() - t0) / 1e6;
        pipeline_cache_save();
        std::lock_guard lk(g_save_lock);
        log("nvn/vk: pipeline cache: the warm-up's merged in (VkResult %d, %.1f ms), the file holds %zu bytes",
            int(merged), ms, g_saved_size);
    }
    p_vkDestroyPipelineCache(vk_device(), fork, nullptr);
}

// The replay's pipeline cache misses (nvn_pipeline.cpp), whatever served them (nvn_pipeline_warm.cpp: a warm-up
// pipeline, another key's, or a new one), and the time they took; and the replay's ASTC decodes.
void note_pipeline_created(uint64_t ns) { g_pipelines++, g_pipeline_ns += ns; }
void note_astc_decoded(uint64_t ns) { g_astc++, g_astc_ns += ns; }

// One line per timing window (nvn_vk.cpp log_timing): what the replay created, and the time it took.
void creation_log(uint64_t frames) {
    if (g_pipelines || g_astc)
        log("nvn/vk: created over %llu frames: %llu pipelines (%.1f ms), %llu ASTC textures decoded (%.1f ms)",
            (unsigned long long)frames, (unsigned long long)g_pipelines, double(g_pipeline_ns) / 1e6,
            (unsigned long long)g_astc, double(g_astc_ns) / 1e6);
    g_pipelines = g_pipeline_ns = g_astc = g_astc_ns = 0;
    static uint64_t windows = 0;
    if (++windows % 10 == 0) {
        save(false);
        capture_log();   // nvn_pipeline_capture.cpp: what the capture file holds, when that changed
        handover_log();  // nvn_pipeline_warm.cpp: how the replay's misses were served
    }
}

}  // namespace nx::nvn
