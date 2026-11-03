// On-screen touch controls for phones (Android, iOS; touch_controls.cpp): the stick that appears where the left thumb
// lands, the buttons of the drawn layout (touch_layout.cpp) or, for games without one, zones on the right half, merged
// into player 1's pad and drawn over the picture by the NVN present pass (plat::touch_overlay).
#pragma once

#include <cstdint>

#include "platform.h"

namespace nx::touch {

enum class Phase { Down, Move, Up };  // Up also for a touch the OS cancelled

// One finger went down, moved or lifted; id tells the fingers apart. x, y: where, in the landscape view the picture
// is presented in, width x height its whole size (bars included, any unit: Android window pixels, iOS points). The
// overlay is drawn across that same view, so x / width and y / height are the drawn controls' coordinates whatever the
// picture's placement. A finger down also brings the overlay back after a controller hid it. Called on the UI thread.
void feed(int64_t id, float x, float y, Phase phase, float width, float height);
// Every finger lifted: the last one went up or the OS cancelled the gesture (Android), which also drops a finger whose
// up was lost.
void release_all();
// ORs the held buttons into p and, while the stick finger is down, sets the left stick (over a controller's).
void merge(plat::PadState& p);
// A physical controller is in use: the overlay hides (until set false or the next finger down).
void set_gamepad_active(bool active);

}  // namespace nx::touch
