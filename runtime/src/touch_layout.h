// Drawn touch controls: the per-game button layout (touch_layout.cpp), hit-tested and drawn by touch_controls.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"

namespace nx::plat {

struct TouchButton {
    float x, y, r;  // centre across the landscape screen (0..1), radius as a fraction of its height
    uint64_t button;
};
// The game's layout (count 0: the game handles touch its own way).
const TouchButton* touch_buttons(size_t& count);
// The button under (x, y) (0..1 across the screen; aspect = width / height), or 0.
uint64_t touch_hit(float x, float y, float aspect);

}  // namespace nx::plat
