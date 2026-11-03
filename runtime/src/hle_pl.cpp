// nn::pl (shared system fonts) at the API level.
//
// On the console the pl sysmodule loads the system fonts (system archives 0100000000000810-814) into shared memory
// in BFTTF form: an 8-byte header {magic, size} and the TrueType data, both XORed with a key. The game hands that
// to nn::fontll::ScalableFontEngineHelper::Decode (sdk+3093c0 in SDK 16), which accepts the data already decoded
// when the first word is the plain magic 0x18029A7F, and then only skips the header.
//
// So any TrueType font works: <data root>/fonts/shared_<type>.ttf (0 standard, 1 simplified Chinese, 2 its
// extension, 3 traditional Chinese, 4 Korean, 5 Nintendo extension), else shared.ttf, else the host's own
// (NX_SHARED_FONT, or a system font on macOS, Android and Windows). The fonts on our own firmware dump can go there once decoded.
//
// iOS has no system font in that list: the Mac's Arial Unicode does not ship there, and its own font files are mostly
// .ttc collections at paths that are not an interface. tools/fonts/collect_host_fonts.py copies the fonts the Mac
// uses into the data folder instead, so the phone renders the same text.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

struct Font {
    uint64_t addr = 0, size = 0;  // header + data, in guest memory
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return v;
    plat::fseek64(f, 0, SEEK_END);
    int64_t n = plat::ftell64(f);
    plat::fseek64(f, 0, SEEK_SET);
    if (n > 0) {
        v.resize(size_t(n));
        v.resize(std::fread(v.data(), 1, v.size(), f));
    }
    std::fclose(f);
    return v;
}

const char* const kTypeNames[6] = {"standard", "simplified Chinese", "simplified Chinese extension",
                                   "traditional Chinese", "Korean", "Nintendo extension"};

// A single font: TrueType outlines (sfnt version 1.0 or 'true') or CFF ('OTTO'). A collection ('ttcf') starts with a
// table of offsets to its fonts instead, which the game's engine would misread.
bool single_font(const std::vector<uint8_t>& ttf) {
    if (ttf.size() < 12) return false;
    uint32_t tag = uint32_t(ttf[0]) << 24 | uint32_t(ttf[1]) << 16 | uint32_t(ttf[2]) << 8 | ttf[3];
    return tag == 0x00010000 || tag == 0x74727565 || tag == 0x4F54544F;
}

const Font& font(uint32_t type) {
    static std::mutex lock;
    static Font fonts[6];
    if (type >= 6) type = 0;
    std::lock_guard lk(lock);
    Font& f = fonts[type];
    if (f.addr) return f;
    std::string root = plat::data_root() + "/fonts/";
    std::vector<std::string> candidates = {root + "shared_" + std::to_string(type) + ".ttf", root + "shared.ttf"};
    if (const char* e = std::getenv("NX_SHARED_FONT")) candidates.push_back(e);
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    // tools/fonts/collect_host_fonts.py copies the first of these that exists: keep the two lists the same.
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    candidates.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(__ANDROID__)
    candidates.push_back("/system/fonts/Roboto-Regular.ttf");
#elif defined(_WIN32)
    // Arial Unicode (Office installs it) covers the most; else Arial, as on the Mac.
    candidates.push_back("C:/Windows/Fonts/arialuni.ttf");
    candidates.push_back("C:/Windows/Fonts/arial.ttf");
#endif
    std::vector<uint8_t> ttf;
    std::string tried;
    for (const auto& path : candidates) {
        ttf = read_file(path);
        if (single_font(ttf)) {
            log("pl: shared font %u (%s) <- %s (%zu bytes)", type, kTypeNames[type], path.c_str(), ttf.size());
            break;
        }
        if (!ttf.empty()) log("pl: %s is not a single TrueType or OpenType font, skipped", path.c_str());
        ttf.clear();
        tried += (tried.empty() ? "" : ", ") + path;
    }
    if (ttf.empty()) log("pl: shared font %u (%s) missing; tried %s", type, kTypeNames[type], tried.c_str());
    f.size = ttf.size() + 8;
    f.addr = kernel_alloc(f.size);
    st<uint32_t>(f.addr, 0x18029A7F);  // plain magic: "already decoded"
    st<uint32_t>(f.addr + 4, uint32_t(ttf.size()));
    if (!ttf.empty()) std::memcpy(reinterpret_cast<void*>(f.addr + 8), ttf.data(), ttf.size());
    return f;
}

void GetSharedFontLoadState(CPU& c) { c.x[0] = 1; }  // SharedFontLoadState_Loaded
void GetSharedFontAddress(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).addr; }
void GetSharedFontSize(CPU& c) { c.x[0] = font(uint32_t(c.x[0])).size; }

}  // namespace

NX_HLE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", GetSharedFontLoadState);
NX_HLE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", GetSharedFontAddress);
NX_HLE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", GetSharedFontSize);

}  // namespace nx
