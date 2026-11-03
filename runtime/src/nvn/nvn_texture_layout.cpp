// Texture storage layout (docs/NVN.md, N4): how many bytes of a memory pool an NVN texture occupies, and how it is
// aligned. The SDK driver computes this through its device's surface-layout object, which only its own
// nvnDeviceInitialize (talking to the GPU kernel driver) sets up, so we compute it here: Maxwell block-linear.
//
// Format facts come from the driver's own format table (0x30 bytes per NVNformat; sdk+0xd34198 in SDK 16.2.0,
// sdk+0xd351a8 in 16.2.12): word 2: block width (byte 0), block height (byte 1); word 4: bytes per block (byte 0).
// It is found by its contents, so no offset per SDK build: the one 8-aligned place in the sdk image where seven
// formats of known shape (RGBA8, RGBA32F, BC1 x2, BC7, ASTC 4x4, ASTC 12x12) have the entries they must have.
// Block linear: the image is cut into GOBs of 64 bytes x 8 rows; GOBs stack vertically into blocks of
// 1..16 GOBs (the block height, picked per mip level from the level's height); a level's rows of blocks are
// 64-byte aligned in width and block-aligned in height. Pre-tiled data (BNTX) is laid out the same way, so the
// sizes must not come out smaller than the real driver's.
#include <algorithm>

#include "nvn.h"

namespace nx::nvn {

namespace {

constexpr uint32_t kFormatCount = 0xd0;

struct FormatInfo {
    uint32_t block_w = 1, block_h = 1, bytes = 4;
};

uint64_t find_format_table() {
    struct Known { uint32_t format, block_w, block_h, bytes; };
    static constexpr Known kKnown[] = {{0x25, 1, 1, 4},  {0x2e, 1, 1, 16}, {0x41, 4, 4, 8},   {0x42, 4, 4, 8},
                                       {0x4d, 4, 4, 16}, {0x79, 4, 4, 16}, {0x86, 12, 12, 16}};
    const LoadedModule* m = module_at(find_symbol("nvnBootstrapLoader"));
    if (!m) fatal("nvn: sdk module not found");
    const uint64_t end = m->base + m->info->data + m->info->data_size - uint64_t(kFormatCount) * 0x30;
    uint64_t found = 0;
    for (uint64_t t = m->base + m->info->ro; t < end; t += 8) {
        bool all = true;
        for (const Known& k : kKnown) {
            const uint32_t* e = reinterpret_cast<const uint32_t*>(t + uint64_t(k.format) * 0x30);
            if ((e[2] & 0xFF) != k.block_w || ((e[2] >> 8) & 0xFF) != k.block_h || (e[4] & 0xFF) != k.bytes) {
                all = false;
                break;
            }
        }
        if (!all) continue;
        if (found) fatal("nvn: two format tables in the sdk (+%llx, +%llx)", (unsigned long long)(found - m->base),
                         (unsigned long long)(t - m->base));
        found = t;
    }
    if (!found) fatal("nvn: format table not found in the sdk");
    log("nvn: format table at sdk+%llx", (unsigned long long)(found - m->base));
    return found;
}

FormatInfo format_info(uint32_t format) {
    static const uint64_t table = find_format_table();
    FormatInfo f;
    if (format >= kFormatCount) return f;
    const uint32_t* e = reinterpret_cast<const uint32_t*>(table + uint64_t(format) * 0x30);
    uint32_t bw = e[2] & 0xFF, bh = (e[2] >> 8) & 0xFF, bytes = e[4] & 0xFF;
    if (bw && bh && bytes) f = {bw, bh, bytes};
    return f;
}

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// Block height in GOBs for a level `rows` block rows tall: the smallest power of two >= rows / 8, at most 16.
uint32_t block_height_gobs(uint32_t rows) {
    uint32_t gobs = div_up(rows, 8), h = 1;
    while (h < gobs && h < 16) h <<= 1;
    return h;
}

struct Layout {
    uint64_t size = 0, alignment = 512;
};

Layout compute(CPU& c, uint64_t builder) {
    uint32_t format = uint32_t(driver(c, "nvnTextureBuilderGetFormat", {builder}));
    uint32_t width = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetWidth", {builder})));
    uint32_t height = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetHeight", {builder})));
    uint32_t depth = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetDepth", {builder})));
    uint32_t levels = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetLevels", {builder})));
    uint32_t samples = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetSamples", {builder})));
    uint32_t stride = uint32_t(driver(c, "nvnTextureBuilderGetStride", {builder}));
    FormatInfo f = format_info(format);
    Layout l;
    if (stride) {  // linear (pitch) texture
        l.size = uint64_t(stride) * div_up(height, f.block_h) * depth;
        l.alignment = 0x100;
        return l;
    }
    uint64_t layer = 0;
    uint32_t bh0 = block_height_gobs(div_up(height, f.block_h));
    for (uint32_t level = 0; level < levels; level++) {
        uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        uint32_t cols = div_up(w, f.block_w), rows = div_up(h, f.block_h);
        uint32_t bh = std::min(bh0, block_height_gobs(rows));
        uint64_t pitch = (uint64_t(cols) * f.bytes + 63) & ~63ull;
        uint64_t tall = (uint64_t(rows) + 8 * bh - 1) / (8 * bh) * (8 * bh);
        layer += pitch * tall;
    }
    uint64_t block = 512ull * bh0;  // one block: 64 bytes x 8 rows x block height
    layer = (layer + block - 1) / block * block;
    l.size = layer * depth * samples;
    l.alignment = block;
    return l;
}

void GetStorageSize(CPU& c) { c.x[0] = compute(c, c.x[0]).size; }            // size_t (const NVNtextureBuilder*)
void GetStorageAlignment(CPU& c) { c.x[0] = compute(c, c.x[0]).alignment; }  // size_t (const NVNtextureBuilder*)
void GetStorageClass(CPU& c) { c.x[0] = 0; }  // NVNstorageClass: only matters for compression / sparse tiles

}  // namespace

uint64_t texture_storage_size(CPU& c, uint64_t builder) { return compute(c, builder).size; }

void format_block(uint32_t format, uint32_t& block_w, uint32_t& block_h, uint32_t& bytes) {
    FormatInfo f = format_info(format);
    block_w = f.block_w, block_h = f.block_h, bytes = f.bytes;
}

NX_HLE("nvnTextureBuilderGetStorageSize", GetStorageSize);
NX_HLE("nvnTextureBuilderGetStorageAlignment", GetStorageAlignment);
NX_HLE("nvnTextureBuilderGetStorageClass", GetStorageClass);
NX_HLE("nvnTextureBuilderGetRawStorageClass", GetStorageClass);

}  // namespace nx::nvn
