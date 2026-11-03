// Drawn touch controls (docs/nvn/ANDROID-PORT.md A4): a fixed layout of round buttons per game, hit-tested by
// touch_controls.cpp (Android, iOS) and drawn by the NVN present pass (nvn_present.cpp) over the picture.
//
// Super Mario Bros. Wonder, landscape: A (jump, the big one) at the bottom right with B (jump), Y (dash, action) and
// X (action) around it; ZR (roll), R, L, + and - along the top. The left half is the stick (touch_controls.cpp): it
// appears where the thumb lands, and pushed nearly all the way it also holds Y, so running needs no second thumb.
// Games without a layout here keep their own touch handling (Hades: zones around the thumb).
//
// Elsewhere (no touch_controls.cpp) touch_overlay() draws nothing, except with NX_OVERLAY_DEMO=1 (the buttons at
// rest), to check the look on the Mac with NX_PRESENT_TEST.
#include "touch_layout.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <nx/game.h>

namespace nx::plat {

namespace {

// x, y: 0..1 across the landscape screen; r: a fraction of the screen height.
const TouchButton kWonder[] = {
    {0.905f, 0.74f, 0.105f, kA},     {0.775f, 0.86f, 0.085f, kB},    {0.775f, 0.60f, 0.085f, kY},
    {0.905f, 0.47f, 0.075f, kX},     {0.955f, 0.25f, 0.055f, kZR},   {0.955f, 0.07f, 0.045f, kR},
    {0.045f, 0.07f, 0.045f, kL},     {0.545f, 0.07f, 0.040f, kPlus}, {0.455f, 0.07f, 0.040f, kMinus},
};

}  // namespace

const TouchButton* touch_buttons(size_t& count) {
    static const bool wonder = std::strcmp(game::kInfo.id, "wonder") == 0;
    count = wonder ? sizeof kWonder / sizeof kWonder[0] : 0;
    return wonder ? kWonder : nullptr;
}

uint64_t touch_hit(float x, float y, float aspect) {
    size_t n = 0;
    const TouchButton* b = touch_buttons(n);
    uint64_t best = 0;
    float best_d = 1e9f;
    for (size_t k = 0; k < n; k++) {
        float d = std::hypot((x - b[k].x) * aspect, y - b[k].y) / b[k].r;
        if (d < 1.35f && d < best_d) best = b[k].button, best_d = d;  // a little past the drawn edge still counts
    }
    return best;
}

#if !defined(__ANDROID__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
size_t touch_overlay(OverlayShape* out, size_t max) {
    static const bool demo = std::getenv("NX_OVERLAY_DEMO") != nullptr;
    if (!demo) return 0;
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    for (; k < n && k < max; k++) out[k] = {b[k].x, b[k].y, b[k].r, 0.22f};
    if (k < max) out[k++] = {0.15f, 0.72f, 0.12f, 0.10f};  // where the stick usually goes
    return k;
}
#endif

}  // namespace nx::plat
