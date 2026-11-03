// Pipeline capture (nvn_pipeline_warm.h): every pipeline the replay creates, recorded by content, and the records read
// back for the warm-up (nvn_pipeline_warm.cpp).
//
// The file: a 16-byte header (magic, version), then records appended as the replay creates pipelines:
//   u32 size, u32 kind, u64 key, <size bytes>, u64 check (hash_bytes over kind, key and the bytes)
// A shader module's bytes are its SPIR-V words and its key their hash; a pipeline's bytes are its create info and its
// key their hash. A pipeline names its modules by hash and carries its layout inline (each set layout's bindings, the
// push constant ranges), so nothing in it depends on the device or the process. Reading stops at the first record cut
// short or failing its check, and the writer writes right after the last whole record and cuts off whatever follows
// it, in place: a crash while writing loses only the records being written, and no record ever follows a broken one.
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nvn_pipeline.h"
#include "nvn_pipeline_warm.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// Magic and version 1 (little endian). A file with another header is started again.
constexpr uint8_t kHeader[16] = {'N', 'X', 'P', 'I', 'P', 'E', 'S', 0, 1, 0, 0, 0, 0, 0, 0, 0};

uint64_t hash_of(const void* p, size_t n) { return hash_bytes(static_cast<const unsigned char*>(p), n); }

struct Pending {
    uint64_t key;
    uint32_t kind;
    std::vector<uint8_t> bytes;  // the whole record, framed
};

// What the replay hands the writer (under `lock`), and the writer's counts. The handles a pipeline refers to are
// known by content: a module by its words' hash, a layout by its recorded bytes.
struct Capture {
    std::mutex lock;
    std::condition_variable wake;  // the writer: records are pending
    std::unordered_map<VkShaderModule, uint64_t> modules;
    std::unordered_map<VkDescriptorSetLayout, std::vector<uint8_t>> sets;
    std::unordered_map<VkPipelineLayout, std::vector<uint8_t>> layouts;
    std::unordered_set<uint64_t> queued;  // keys handed to the writer this run, and those in the file once read
    std::vector<Pending> pending;
    std::set<std::string> refusals;  // logged once each
    uint64_t added = 0, refused = 0;  // pipelines this run: written to the file, not recordable
    uint64_t file_pipelines = 0, file_modules = 0, file_bytes = 0;
    uint64_t logged_refused = ~0ull, logged_file = ~0ull;
    std::string path;
    bool warm = false;  // the warm-up may run (a pipeline cache to merge into)
};

Capture& capture() {
    static Capture& c = *new Capture;  // never destroyed: its threads outlive static destructors
    return c;
}

struct Readback;

// ---- create infos to bytes and back ---------------------------------------------------------------------------

// The extension structs the runtime chains: dynamic rendering's formats (graphics_pipeline) and clip-space depth
// -1..1 (its viewport state). Anything else refuses the record rather than have it rebuilt without the struct.
union Ext {
    VkBaseOutStructure base;
    VkPipelineRenderingCreateInfo rendering;
    VkPipelineViewportDepthClipControlCreateInfoEXT clip;
};

template <class IO>
void visit_ext(IO& io, VkStructureType type, void* s) {
    if (type == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
        auto& r = *static_cast<VkPipelineRenderingCreateInfo*>(s);
        io.vals(r.viewMask, r.depthAttachmentFormat, r.stencilAttachmentFormat);
        io.list(r.pColorAttachmentFormats, r.colorAttachmentCount);
    } else if (type == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT) {
        io.vals(static_cast<VkPipelineViewportDepthClipControlCreateInfoEXT*>(s)->negativeOneToOne);
    } else {
        io.refuse("an extension struct it does not know");
    }
}

// Writes a create info's bytes. The visit() templates below drive Put and Get alike, so that the two cannot disagree
// on the layout; Put only reads what it is given (the const_casts serve the shared signatures).
struct Put {
    std::vector<uint8_t>& out;
    const Capture& c;
    const char* refused = nullptr;  // what could not be recorded: the record is dropped

    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    template <class... T>
    void vals(T&... v) { (bytes(&v, sizeof v), ...); }
    template <class T>
    T* items(const T*& p, uint32_t& n) {  // a count, then elements the caller visits
        uint32_t count = p ? n : 0;
        bytes(&count, 4);
        return const_cast<T*>(p);
    }
    template <class T>
    void list(const T*& p, uint32_t& n) { bytes(items(p, n), p ? n * sizeof(T) : 0); }  // elements without pointers
    template <class T>
    T* object(const T*& p) {  // an optional struct: its sType, 0 for none
        VkStructureType type = p ? p->sType : VkStructureType(0);
        bytes(&type, 4);
        return const_cast<T*>(p);
    }
    void chain(const void*& next) {  // each struct's sType and fields, then 0
        for (auto* s = static_cast<const VkBaseInStructure*>(next); s; s = s->pNext) {
            VkStructureType type = s->sType;
            vals(type);
            visit_ext(*this, type, const_cast<VkBaseInStructure*>(s));
        }
        VkStructureType end{};
        vals(end);
    }
    void name(const char*& s) {
        uint32_t n = uint32_t(std::strlen(s));
        bytes(&n, 4);
        bytes(s, n);
    }
    template <class Map, class H>
    void blob(const Map& map, H h) {
        auto it = map.find(h);
        uint32_t n = it != map.end() ? uint32_t(it->second.size()) : 0;
        bytes(&n, 4);
        if (it != map.end()) bytes(it->second.data(), n);
        else refuse("a layout that was not recorded");
    }
    void handle(VkShaderModule& m) {
        auto it = c.modules.find(m);
        if (it == c.modules.end()) refuse("a shader module that was not recorded");
        uint64_t key = it != c.modules.end() ? it->second : 0;
        bytes(&key, 8);
    }
    void handle(VkDescriptorSetLayout& l) { blob(c.sets, l); }
    void handle(VkPipelineLayout& l) { blob(c.layouts, l); }
    void refuse(const char* why) { refused = refused ? refused : why; }
};

// Reads them back into structs allocated from an arena, making the modules and layouts they name (Readback).
struct Get {
    const uint8_t* p;
    const uint8_t* end;
    Arena& arena;
    Readback& rb;
    bool bad = false;  // cut short, or a module or layout that could not be made

    void bytes(void* d, size_t n) {  // zeros past the end
        bool fits = size_t(end - p) >= n;
        bad = bad || !fits;
        if (n) fits ? std::memcpy(d, p, n) : std::memset(d, 0, n);
        p += fits ? n : 0;
    }
    template <class... T>
    void vals(T&... v) { (bytes(&v, sizeof v), ...); }
    uint32_t count() {  // never more than the bytes left, whatever a damaged record says
        uint32_t n = 0;
        bytes(&n, 4);
        if (n > size_t(end - p)) bad = true, n = 0;
        return n;
    }
    template <class T>
    T* items(const T*& out, uint32_t& n) {
        n = count();
        T* a = arena.make<T>(n);
        out = a;
        return a;
    }
    template <class T>
    void list(const T*& out, uint32_t& n) {
        T* a = items(out, n);  // sets n: not in the same call as n's use (argument order is unspecified)
        bytes(a, n * sizeof(T));
    }
    template <class T>
    T* object(const T*& out) {
        VkStructureType type{};
        bytes(&type, 4);
        T* o = type && !bad ? arena.make<T>(1) : nullptr;
        if (o) o->sType = type;
        out = o;
        return o;
    }
    void chain(const void*& next) {
        VkStructureType type{};
        for (VkBaseOutStructure* last = nullptr; vals(type), type && !bad;) {
            auto* e = &arena.make<Ext>(1)->base;
            e->sType = type;
            visit_ext(*this, type, e);
            if (last) last->pNext = e;
            else next = e;
            last = e;
        }
    }
    void name(const char*& s) {
        uint32_t n = count();
        char* c = arena.make<char>(n + 1);
        bytes(c, n);
        s = c;
    }
    std::pair<const uint8_t*, size_t> blob() {
        uint32_t n = count();
        const uint8_t* at = p;
        p += n;
        return {at, n};
    }
    void handle(VkShaderModule& m);
    void handle(VkDescriptorSetLayout& l);
    void handle(VkPipelineLayout& l);
    void refuse(const char*) {}
};

template <class IO>
void visit(IO& io, VkPipelineShaderStageCreateInfo& s) {
    io.vals(s.sType, s.flags, s.stage);
    io.chain(s.pNext);
    io.handle(s.module);
    io.name(s.pName);
    if (s.pSpecializationInfo) io.refuse("specialization constants");
}

template <class IO>
void visit(IO& io, VkDescriptorSetLayoutCreateInfo& ci) {
    io.chain(ci.pNext);
    io.vals(ci.flags);
    VkDescriptorSetLayoutBinding* b = io.items(ci.pBindings, ci.bindingCount);
    for (uint32_t k = 0; b && k < ci.bindingCount; k++) {
        io.vals(b[k].binding, b[k].descriptorType, b[k].descriptorCount, b[k].stageFlags);
        if (b[k].pImmutableSamplers) io.refuse("immutable samplers");
    }
}

template <class IO>
void visit(IO& io, VkPipelineLayoutCreateInfo& ci) {
    io.chain(ci.pNext);
    io.vals(ci.flags);
    VkDescriptorSetLayout* sets = io.items(ci.pSetLayouts, ci.setLayoutCount);
    for (uint32_t k = 0; sets && k < ci.setLayoutCount; k++) io.handle(sets[k]);
    io.list(ci.pPushConstantRanges, ci.pushConstantRangeCount);
}

template <class IO>
void visit(IO& io, VkGraphicsPipelineCreateInfo& ci) {
    io.chain(ci.pNext);
    io.vals(ci.flags, ci.subpass, ci.basePipelineIndex);
    if (ci.renderPass) io.refuse("a render pass object (NX_RENDER_PASS_OBJECTS)");
    if (ci.basePipelineHandle) io.refuse("a base pipeline");
    VkPipelineShaderStageCreateInfo* st = io.items(ci.pStages, ci.stageCount);
    for (uint32_t k = 0; st && k < ci.stageCount; k++) visit(io, st[k]);
    // First, as it decides whether viewports and scissors are part of the pipeline.
    bool dynamic_viewport = false, dynamic_scissor = false;
    if (auto* d = io.object(ci.pDynamicState)) {
        io.chain(d->pNext);
        io.vals(d->flags);
        io.list(d->pDynamicStates, d->dynamicStateCount);
        for (uint32_t k = 0; d->pDynamicStates && k < d->dynamicStateCount; k++) {
            dynamic_viewport = dynamic_viewport || d->pDynamicStates[k] == VK_DYNAMIC_STATE_VIEWPORT;
            dynamic_scissor = dynamic_scissor || d->pDynamicStates[k] == VK_DYNAMIC_STATE_SCISSOR;
        }
    }
    if (auto* v = io.object(ci.pVertexInputState)) {
        io.chain(v->pNext);
        io.vals(v->flags);
        io.list(v->pVertexBindingDescriptions, v->vertexBindingDescriptionCount);
        io.list(v->pVertexAttributeDescriptions, v->vertexAttributeDescriptionCount);
    }
    if (auto* a = io.object(ci.pInputAssemblyState)) {
        io.chain(a->pNext);
        io.vals(a->flags, a->topology, a->primitiveRestartEnable);
    }
    if (io.object(ci.pTessellationState)) io.refuse("tessellation");
    if (auto* v = io.object(ci.pViewportState)) {  // the counts only: the runtime's viewports are dynamic
        io.chain(v->pNext);
        io.vals(v->flags, v->viewportCount, v->scissorCount);
        if ((v->pViewports && !dynamic_viewport) || (v->pScissors && !dynamic_scissor)) io.refuse("static viewports");
    }
    if (auto* r = io.object(ci.pRasterizationState)) {
        io.chain(r->pNext);
        io.vals(r->flags, r->depthClampEnable, r->rasterizerDiscardEnable, r->polygonMode, r->cullMode, r->frontFace,
                r->depthBiasEnable, r->depthBiasConstantFactor, r->depthBiasClamp, r->depthBiasSlopeFactor,
                r->lineWidth);
    }
    if (auto* m = io.object(ci.pMultisampleState)) {
        io.chain(m->pNext);
        io.vals(m->flags, m->rasterizationSamples, m->sampleShadingEnable, m->minSampleShading,
                m->alphaToCoverageEnable, m->alphaToOneEnable);
        if (m->pSampleMask) io.refuse("a sample mask");
    }
    if (auto* d = io.object(ci.pDepthStencilState)) {
        io.chain(d->pNext);
        io.vals(d->flags, d->depthTestEnable, d->depthWriteEnable, d->depthCompareOp, d->depthBoundsTestEnable,
                d->stencilTestEnable, d->front, d->back, d->minDepthBounds, d->maxDepthBounds);
    }
    if (auto* b = io.object(ci.pColorBlendState)) {
        io.chain(b->pNext);
        io.vals(b->flags, b->logicOpEnable, b->logicOp, b->blendConstants);
        io.list(b->pAttachments, b->attachmentCount);
    }
    io.handle(ci.layout);
}

template <class IO>
void visit(IO& io, VkComputePipelineCreateInfo& ci) {
    io.chain(ci.pNext);
    io.vals(ci.flags, ci.basePipelineIndex);
    if (ci.basePipelineHandle) io.refuse("a base pipeline");
    visit(io, ci.stage);
    io.handle(ci.layout);
}

// ---- the capture ----------------------------------------------------------------------------------------------

std::vector<uint8_t> frame(uint32_t kind, uint64_t key, const void* body, size_t size) {
    std::vector<uint8_t> r(24 + size);
    uint32_t head[4] = {uint32_t(size), kind, uint32_t(key), uint32_t(key >> 32)};
    std::memcpy(r.data(), head, 16);
    if (size) std::memcpy(r.data() + 16, body, size);
    uint64_t check = hash_of(r.data() + 4, 12 + size);
    std::memcpy(r.data() + 16 + size, &check, 8);
    return r;
}

void queue_locked(Capture& c, uint32_t kind, uint64_t key, const void* body, size_t size) {
    if (!c.queued.insert(key).second) return;
    c.pending.push_back({key, kind, frame(kind, key, body, size)});
    c.wake.notify_one();
}

void refusal_locked(Capture& c, const char* why) {
    if (c.refusals.insert(why).second) log("nvn/vk: pipeline capture: one with %s is not recorded", why);
}

// A layout's or a pipeline's bytes, under the capture lock: false (and logged once) if it cannot be recorded.
template <class CI>
bool put_locked(Capture& c, const CI& ci, std::vector<uint8_t>& b) {
    Put put{b, c};
    visit(put, const_cast<CI&>(ci));
    if (put.refused) refusal_locked(c, put.refused);
    return !put.refused;
}

// A replay create info's record and key (capture_record).
template <class CI>
bool record_of(const CI& ci, std::vector<uint8_t>& body, uint64_t& key) {
    Capture& c = capture();
    std::lock_guard lk(c.lock);
    if (!put_locked(c, ci, body)) return c.refused++, false;
    key = hash_of(body.data(), body.size());
    return true;
}

// Cuts the file back to `end` bytes (its whole records) in place: writing a copy and renaming it over the file left no
// file for a moment, and none at all when the copy could not be written.
void cut(const std::string& path, uint64_t end) {
    std::filesystem::path p(std::u8string(path.begin(), path.end()));  // UTF-8, as plat::fopen_utf8 takes it
    std::error_code err;
    if (std::filesystem::file_size(p, err) > end && !err) std::filesystem::resize_file(p, end, err);
}

// The file's whole records, pointing into `data`, and in `end` where they end (0: no header of this version, so the
// writer starts the file again). A tail cut short or damaged is cut from the file.
std::vector<Record> read_records(const std::string& path, std::vector<uint8_t>& data, uint64_t& end) {
    if (FILE* f = plat::fopen_utf8(path, "rb")) {
        plat::fseek64(f, 0, SEEK_END);
        int64_t n = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_SET);
        if (n > 0) data.resize(size_t(n)), data.resize(std::fread(data.data(), 1, data.size(), f));
        std::fclose(f);
    }
    std::vector<Record> recs;
    if (data.empty()) return recs;
    bool ours = data.size() >= sizeof kHeader && std::memcmp(data.data(), kHeader, sizeof kHeader) == 0;
    size_t pos = ours ? sizeof kHeader : 0;
    while (ours && data.size() - pos >= 24) {
        uint32_t head[4];
        uint64_t check = 0;
        std::memcpy(head, &data[pos], 16);
        size_t size = head[0];
        if (size > data.size() - pos - 24) break;
        std::memcpy(&check, &data[pos + 16 + size], 8);
        if (check != hash_of(&data[pos + 4], 12 + size)) break;
        recs.push_back({head[1], head[2] | uint64_t(head[3]) << 32, &data[pos + 16], size});
        pos += 24 + size;
    }
    end = pos;
    if (pos == data.size()) return recs;
    log("nvn/vk: pipeline capture %s: %s, %zu records kept", path.c_str(),
        ours ? "the last record is cut short or damaged" : "not a capture of this version", recs.size());
    data.resize(pos);  // shrinking: the records still point at valid bytes
    cut(path, pos);
    return recs;
}

// Writes the records the file does not hold yet, in the order they were captured, at `end` (after the last whole
// record, over anything a failed write left there), then cuts what follows: a write that fails or is cut short by a
// crash leaves the file whole up to its last whole record, and the next batch goes there.
void append(Capture& c, std::unordered_set<uint64_t>& known, uint64_t& end, const std::vector<Pending>& batch) {
    std::vector<uint8_t> out;
    if (end == 0) out.assign(kHeader, kHeader + sizeof kHeader);
    uint64_t pipelines = 0, modules = 0;
    for (const Pending& r : batch)
        if (known.insert(r.key).second) {
            out.insert(out.end(), r.bytes.begin(), r.bytes.end());
            (r.kind == kRecordModule ? modules : pipelines)++;
        }
    if (!pipelines && !modules) return;
    FILE* f = plat::fopen_utf8(c.path, "r+b");
    if (!f) f = plat::fopen_utf8(c.path, "w+b");  // no file yet
    bool ok = f && plat::fseek64(f, int64_t(end), SEEK_SET) == 0;
    ok = ok && std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = f && std::fclose(f) == 0 && ok;
    if (ok) end += out.size();
    else log("nvn/vk: pipeline capture: writing %s failed", c.path.c_str());
    cut(c.path, end);
    std::lock_guard lk(c.lock);
    if (ok) c.file_pipelines += pipelines, c.file_modules += modules, c.added += pipelines;
    c.file_bytes = end;
}
// ---- reading records back -------------------------------------------------------------------------------------

// The file as read at start, for the warm-up: its records point into `data`. The modules and layouts the pipelines name
// are made as a record first needs them, once for every thread.
struct Readback {
    std::vector<uint8_t> data;
    std::unordered_map<uint64_t, Record> words;  // module records by key
    std::recursive_mutex lock;  // the maps below (a layout is made while its set layouts are looked up)
    std::unordered_map<uint64_t, VkShaderModule> modules;
    std::unordered_map<uint64_t, VkDescriptorSetLayout> sets;
    std::unordered_map<uint64_t, VkPipelineLayout> layouts;

    VkShaderModule module(uint64_t key) {
        std::lock_guard lk(lock);
        if (auto it = modules.find(key); it != modules.end()) return it->second;
        auto w = words.find(key);
        if (w == words.end() || w->second.size % 4) return VK_NULL_HANDLE;
        std::vector<uint32_t> code(w->second.size / 4);  // aligned, as pCode must be
        std::memcpy(code.data(), w->second.body, w->second.size);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = w->second.size;
        ci.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
        return modules[key] = m;
    }
    // A set layout or a pipeline layout from its recorded bytes, made once for every thread.
    template <class H, class CI, class Create>
    H make(std::unordered_map<uint64_t, H>& made, CI ci, std::pair<const uint8_t*, size_t> bytes, Create create) {
        uint64_t key = hash_of(bytes.first, bytes.second);
        std::lock_guard lk(lock);
        if (auto it = made.find(key); it != made.end()) return it->second;
        Arena arena;
        Get in{bytes.first, bytes.first + bytes.second, arena, *this};
        visit(in, ci);
        H h = VK_NULL_HANDLE;
        if (in.bad || create(vk_device(), &ci, nullptr, &h) != VK_SUCCESS) return VK_NULL_HANDLE;
        return made[key] = h;
    }
};

Readback& readback() {
    static Readback& r = *new Readback;  // never destroyed: the warm-up's threads use it
    return r;
}

void Get::handle(VkShaderModule& m) {
    uint64_t key = 0;
    vals(key);
    m = bad ? VK_NULL_HANDLE : rb.module(key);
    bad = bad || !m;
}
void Get::handle(VkDescriptorSetLayout& l) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    auto b = blob();
    l = bad ? VK_NULL_HANDLE : rb.make(rb.sets, ci, b, p_vkCreateDescriptorSetLayout);
    bad = bad || !l;
}
void Get::handle(VkPipelineLayout& l) {
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    auto b = blob();
    l = bad ? VK_NULL_HANDLE : rb.make(rb.layouts, ci, b, p_vkCreatePipelineLayout);
    bad = bad || !l;
}

template <class CI>
bool read_into(const Record& r, Arena& arena, CI& ci) {
    Get in{r.body, r.body + r.size, arena, readback()};
    visit(in, ci);
    return !in.bad;
}

// Reads the file (off the replay thread), hands its pipelines to the warm-up, then appends what the replay captures.
void capture_thread(void*) {
    lower_priority();
    plat::set_thread_name("nvn capture");
    Capture& c = capture();
    Readback& rb = readback();  // the warm-up's threads, which read it, start after this has filled it
    std::unordered_set<uint64_t> known;
    std::vector<Record> pipelines;
    uint64_t end = 0;
    for (const Record& r : read_records(c.path, rb.data, end)) {
        known.insert(r.key);
        if (r.kind == kRecordModule) rb.words[r.key] = r;
        else if (r.kind == kRecordGraphics || r.kind == kRecordCompute) pipelines.push_back(r);
    }
    {
        std::lock_guard lk(c.lock);
        c.queued.insert(known.begin(), known.end());  // what the file holds is not framed and queued again
        c.file_pipelines = pipelines.size(), c.file_modules = rb.words.size(), c.file_bytes = end;
    }
    log("nvn/vk: pipeline capture %s: %zu pipelines, %zu shader modules (%.1f MB)", c.path.c_str(), pipelines.size(),
        rb.words.size(), double(rb.data.size()) / 1e6);
    if (!c.warm || !warm_start(std::move(pipelines))) records_done();
    for (;;) {
        {
            std::unique_lock lk(c.lock);
            c.wake.wait(lk, [&] { return !c.pending.empty(); });
        }
        plat::sleep_ns(250'000'000);  // a burst of new pipelines goes out in one append
        std::vector<Pending> batch;
        {
            std::lock_guard lk(c.lock);
            batch.swap(c.pending);
        }
        append(c, known, end, batch);
    }
}

}  // namespace

void capture_module(VkShaderModule module, const std::vector<uint32_t>& words) {
    uint64_t key = hash_of(words.data(), words.size() * 4);
    Capture& c = capture();
    std::lock_guard lk(c.lock);
    c.modules[module] = key;
    queue_locked(c, kRecordModule, key, words.data(), words.size() * 4);
}

void capture_set_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutCreateInfo& ci) {
    Capture& c = capture();
    std::lock_guard lk(c.lock);
    if (std::vector<uint8_t> b; put_locked(c, ci, b)) c.sets[layout] = std::move(b);
}

void capture_pipeline_layout(VkPipelineLayout layout, const VkPipelineLayoutCreateInfo& ci) {
    Capture& c = capture();
    std::lock_guard lk(c.lock);
    if (std::vector<uint8_t> b; put_locked(c, ci, b)) c.layouts[layout] = std::move(b);
}

bool capture_record(const VkGraphicsPipelineCreateInfo& ci, std::vector<uint8_t>& body, uint64_t& key) {
    return record_of(ci, body, key);
}
bool capture_record(const VkComputePipelineCreateInfo& ci, std::vector<uint8_t>& body, uint64_t& key) {
    return record_of(ci, body, key);
}

void capture_add(uint32_t kind, uint64_t key, const std::vector<uint8_t>& body) {
    Capture& c = capture();
    std::lock_guard lk(c.lock);
    queue_locked(c, kind, key, body.data(), body.size());
}

bool read_record(const Record& r, Arena& arena, VkGraphicsPipelineCreateInfo& ci) { return read_into(r, arena, ci); }
bool read_record(const Record& r, Arena& arena, VkComputePipelineCreateInfo& ci) { return read_into(r, arena, ci); }

void records_done() {
    Readback& rb = readback();
    std::lock_guard lk(rb.lock);
    for (auto& [key, m] : rb.modules) p_vkDestroyShaderModule(vk_device(), m, nullptr);
    rb.modules.clear(), rb.words.clear();
    std::vector<uint8_t>().swap(rb.data);
}

void pipeline_capture_start(bool warm) {
    Capture& c = capture();
    c.path = plat::data_root() + "/pipeline_capture.bin";
    c.warm = warm;
    plat::start_thread(capture_thread, nullptr, 1ull << 20);
}

void capture_log() {
    Capture& c = capture();
    std::lock_guard lk(c.lock);
    if (c.refused == c.logged_refused && c.file_pipelines == c.logged_file) return;
    c.logged_refused = c.refused, c.logged_file = c.file_pipelines;
    log("nvn/vk: pipeline capture: %llu new create infos written this run, %llu not recordable; the file holds %llu "
        "pipelines and %llu shader modules (%.1f MB)",
        (unsigned long long)c.added, (unsigned long long)c.refused, (unsigned long long)c.file_pipelines,
        (unsigned long long)c.file_modules, double(c.file_bytes) / 1e6);
}

}  // namespace nx::nvn
