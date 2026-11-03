// Host runtime: memory map, module loader/linker, dispatch, HLE registry, logging.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <nx/cpu.h>
#include <nx/module.h>

namespace nx {

// ---- logging --------------------------------------------------------------------------------------------------

void log(const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);
extern bool g_trace;  // verbose tracing (NX_TRACE=1)

// ---- address space --------------------------------------------------------------------------------------------
// Guest pointers are host pointers. Everything the guest can see lives in reserved ranges. The modules sit at a fixed
// address (NX_BASE, compiled into the generated code); memory_init places every other region and the guest learns
// them from svcGetInfo, so the layout can follow what the host leaves free. macOS 27 and iOS 27 keep almost all of
// 0x1_8000_0000..0x70_0000_0000 for the shared cache and the GPU, and macOS puts a 24 GiB heap range at a random
// place in 0x70..0x80_0000_0000 at every launch; Windows and Android use fixed places (memory.cpp).

constexpr uint64_t kModuleBase = NX_BASE;  // main; sdk at +0x10000000 (NX_BASE, nx/cpu.h)
constexpr uint64_t kModuleSize = 0x20000000ull;

struct GuestLayout {
    uint64_t space_base = 0, space_size = 0;  // the ASLR region (svcGetInfo 12/13): every region below lies in it
    uint64_t heap_base = 0, heap_max = 0;     // svcSetHeapSize grows from heap_base, up to heap_max
    uint64_t alias_base = 0, alias_size = 0;  // nn::os VAMM, mapped memory
    uint64_t stack_base = 0, stack_size = 0;  // thread stacks (MapMemory), shared and transfer memory
    uint64_t kernel_base = 0, kernel_size = 0;  // memory we hand out: TLS pages, main thread stack, shared memory
    uint64_t thunk_base = 0, thunk_size = 0;    // HLE function addresses (reserved, never mapped)
};
extern GuestLayout g_layout;  // set by memory_init, before anything else runs guest code
// Inside some region of the guest (a cheap sanity check for pointers read from guest memory).
inline bool is_guest(uint64_t p) { return p - g_layout.space_base < g_layout.space_size; }

void memory_init();
bool commit(uint64_t addr, uint64_t size);   // make [addr, addr+size) read/write
void decommit(uint64_t addr, uint64_t size);
uint64_t kernel_alloc(uint64_t size, uint64_t align = 0x1000);  // from the kernel region

// Horizon memory states for QueryMemory.
enum MemState : uint32_t {
    kMemFree = 0x00,
    kMemCode = 0x03,
    kMemCodeData = 0x04,
    kMemHeap = 0x05,
    kMemStatic = 0x0C,  // module ro/data seen as static
    kMemStack = 0x0B,
    kMemThreadLocal = 0x0C,
    kMemAlias = 0x0E,
    kMemInaccessible = 0x10,
};
void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm);
struct VmInfo {
    uint64_t addr, size;
    uint32_t state, perm;
};
VmInfo vm_query(uint64_t addr);

// ---- modules --------------------------------------------------------------------------------------------------

struct LoadedModule {
    const ModuleInfo* info;
    uint64_t base, size;
    Fn* table;  // text offset / 4 -> function
};

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir);
uint64_t find_symbol(const char* name);  // 0 when not found
const LoadedModule* module_at(uint64_t addr);
std::string describe(uint64_t addr);  // "sdk+0x1234 (symbol+0x10)"

// ---- HLE ------------------------------------------------------------------------------------------------------

using HleFn = void (*)(CPU&);
struct HleRegistrar {
    HleRegistrar(const char* name, HleFn fn);
};
#define NX_HLE_CAT2(a, b) a##b
#define NX_HLE_CAT(a, b) NX_HLE_CAT2(a, b)
#define NX_HLE(sym, fn) static ::nx::HleRegistrar NX_HLE_CAT(hle_reg_, __LINE__)(sym, fn)

uint64_t hle_address(const char* name);  // thunk address; unknown names get a logging stub
uint64_t hle_address_or(const char* name, HleFn fallback);  // unknown names get `fallback`
const char* hle_current_name();  // inside an HLE function: the name its thunk was made for
HleFn hle_lookup(const char* name);

// Call guest code at `addr` on context c (args already in x0..).
inline void guest_call(CPU& c, uint64_t addr) { call(c, addr); }

// ---- kernel ---------------------------------------------------------------------------------------------------

extern std::atomic<uint64_t> g_frame;  // frames presented so far (vkQueuePresentKHR)

void kernel_init();
uint32_t create_main_thread(uint64_t stack_size);  // the calling host thread becomes it
CPU& current_cpu();
uint64_t system_tick();
// A file the game can open under a mount (e.g. "Content/Mods/X/a.lua" for Content:/Mods/X/a.lua), served from
// memory; it takes precedence over a real file of the same path (hle_fs.cpp).
void add_virtual_file(const std::string& path, std::string data);
// The contents of a real game file under the data folder ("" if missing).
std::string read_game_file(const std::string& relative_to_romfs);

}  // namespace nx
