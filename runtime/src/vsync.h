// The display's vsync timeline, shared by hle_vi.cpp (the game's vsync event) and nvn_queue.cpp (present pacing).
#pragma once

#include <cstdint>

namespace nx {

constexpr uint64_t kVsyncNs = 16'666'667;  // the Switch's 60 Hz

// The plat::monotonic_ns() time of the latest display vsync handed to the game (plat::vsync_source, hle_vi.cpp); 0
// without a display source (hle_vi.cpp's own 60 Hz timer drives the game's vsync) and before its first tick.
uint64_t display_vsync_ns();

}  // namespace nx
