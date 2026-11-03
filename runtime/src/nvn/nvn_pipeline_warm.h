// Pipeline capture, warm-up and hand-over. The replay compiles each pipeline the first time a draw needs it: on a phone
// whose caches are cold that was 20 s of compiling over a first session, in multi-second hitches, against 0.6 s once
// MoltenVK's VkPipelineCache and the OS's Metal cache held them. So every pipeline the replay creates is recorded,
// device-independently, in <data root>/pipeline_capture.bin (nvn_pipeline_capture.cpp), and at the next start a
// background thread rebuilds every record while the title screen shows and keeps the pipelines (nvn_pipeline_warm.cpp):
// when the replay needs a pipeline whose create info a record holds, it takes the warm-up's instead of compiling its
// own. A capture made on the Mac seeds the phone (tools/wonder_push_ios.sh).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "nvn_vk.h"

namespace nx::nvn {

// ---- nvn_pipeline.cpp ----

// The objects a pipeline refers to, recorded by content as nvn_pipeline.cpp creates them (after a successful
// vkCreate*): a module by the 64-bit hash of its final words (each distinct module is written once), a layout by
// its bindings.
void capture_module(VkShaderModule module, const std::vector<uint32_t>& words);
void capture_set_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutCreateInfo& ci);
void capture_pipeline_layout(VkPipelineLayout layout, const VkPipelineLayoutCreateInfo& ci);

// The pipeline for a create info the replay's own caches have none for: the warm-up's if it built one from the same
// create info (waited for while a warm-up thread builds it), the one another key with the same create info got, or a
// new one, created into the pipeline cache and recorded for the next start. Null if creation failed. The replay binds
// it with its own layout, which may be another object than the one it was built with but is defined identically (a
// record holds its layout's whole definition), and so compatible for Vulkan.
VkPipeline replay_pipeline(const VkGraphicsPipelineCreateInfo& ci);
VkPipeline replay_pipeline(const VkComputePipelineCreateInfo& ci);

// ---- nvn_pipeline_cache.cpp ----

// Once, from pipeline_cache() when it is first asked for (`warm` false with NX_PIPELINE_CACHE=0): reads the file,
// starts its writer, and unless NX_PIPELINE_WARM=0 the warm-up.
void pipeline_capture_start(bool warm);
// What the file holds, and how the replay's misses were served, each logged when it changed (with the pipeline
// cache's periodic save).
void capture_log();
void handover_log();

// Implemented in nvn_pipeline_cache.cpp. The replay creates into the pipeline cache; the warm-up builds into a fork of
// it, a cache of its own seeded with what the pipeline cache holds (MoltenVK holds a cache's lock while it compiles:
// with one cache, the replay waited behind the warm-up's compiles), joined back once the warm-up ends: merged in
// (vkMergePipelineCaches), destroyed, and the pipeline cache saved.
VkResult pipeline_cache_create(const VkGraphicsPipelineCreateInfo& ci, VkPipeline* p);
VkResult pipeline_cache_create(const VkComputePipelineCreateInfo& ci, VkPipeline* p);
VkPipelineCache pipeline_cache_fork();
void pipeline_cache_join(VkPipelineCache fork);

// ---- between nvn_pipeline_capture.cpp and nvn_pipeline_warm.cpp ----

enum RecordKind : uint32_t { kRecordModule = 1, kRecordGraphics = 2, kRecordCompute = 3 };
struct Record {
    uint32_t kind;
    uint64_t key;         // the hash of its bytes
    const uint8_t* body;  // in the file's bytes as read at start
    size_t size;
};

// A replay create info's record and its key, false (and logged once per reason) if it cannot be recorded.
bool capture_record(const VkGraphicsPipelineCreateInfo& ci, std::vector<uint8_t>& body, uint64_t& key);
bool capture_record(const VkComputePipelineCreateInfo& ci, std::vector<uint8_t>& body, uint64_t& key);
// A record whose pipeline the replay created: appended to the file unless it holds it already.
void capture_add(uint32_t kind, uint64_t key, const std::vector<uint8_t>& body);

// Storage for the structs of a record read back: zeroed, freed together.
struct Arena {
    std::vector<std::unique_ptr<std::byte[]>> blocks;
    template <class T>
    T* make(size_t n) {
        if (!n) return nullptr;
        blocks.push_back(std::make_unique<std::byte[]>(n * sizeof(T)));
        return reinterpret_cast<T*>(blocks.back().get());
    }
};
// A pipeline record of the file read back into a create info (its structs from `arena`), with the shader modules and
// layouts it names, each made once for every thread. False: damaged, or a module or layout could not be made.
bool read_record(const Record& r, Arena& arena, VkGraphicsPipelineCreateInfo& ci);
bool read_record(const Record& r, Arena& arena, VkComputePipelineCreateInfo& ci);
// Once the warm-up is over (or not wanted): the modules made for it destroyed and the file's bytes freed, so no Record
// may be read after it. The layouts stay for the process, as the pipelines built with them do.
void records_done();

// From the capture thread once it has read the file: the pipeline records in the order they were captured. False if
// the warm-up does not take them (NX_PIPELINE_WARM=0, or none).
bool warm_start(std::vector<Record> pipelines);
// The capture's and the warm-up's threads run below the game's (nvn_pipeline_warm.cpp).
void lower_priority();

}  // namespace nx::nvn
