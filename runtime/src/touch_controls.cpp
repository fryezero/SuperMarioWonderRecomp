// Touch controls on the phones (Android, iOS), moved out of platform_android_input.cpp so both share them.
//
// With a drawn layout (touch_layout.cpp, Wonder): a finger on one of its buttons holds it; anywhere else on the left
// half it is the stick, relative to where the thumb landed (a push of 12% of the screen height is full tilt), and
// pushed nearly all the way it also holds Y (Wonder: dash), so running needs no second thumb on the right side.
// Without one (Hades): the left half is the stick and the right half four buttons in a diamond around the thumb
// (Y attack up-left, X special up-right, B dash down-left, A interact down-right), top corners L / R, top centre +.
//
// Fingers are kept in view units and turned into screen fractions with the view's size at the last feed() when read,
// as the present pass draws the overlay in fractions of the whole screen (bars included). feed() runs on the UI
// thread, merge() on the game's, touch_overlay() on the render thread: one mutex guards it all.
//
// Built on Android and iOS only; touch_layout.cpp has the touch_overlay() for everything else, under the opposite
// condition, so whichever files a build lists there is exactly one.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)

#include "touch_controls.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "touch_layout.h"

namespace nx::touch {

namespace {

struct Finger {
    int64_t id;
    float x0, y0, x, y;  // where it went down and where it is now (view units)
    bool stick;          // the stick finger, else it holds `button` (0: none)
    uint64_t button;
};

std::mutex g_mutex;
std::vector<Finger> g_fingers;
float g_w = 1, g_h = 1;  // the view's size at the last feed()
bool g_gamepad = false;  // the last input came from a controller: the drawn controls hide

// No drawn layout: the right half's zones, at (x, y) in fractions of the screen.
uint64_t zone_button(float x, float y) {
    if (y < 0.2f) return x < 0.6f ? (x < 0.45f ? plat::kL : plat::kPlus) : (x > 0.85f ? plat::kR : plat::kZR);
    bool up = y < 0.6f, right = x > 0.8f;
    if (up) return right ? plat::kX : plat::kY;
    return right ? plat::kA : plat::kB;
}

}  // namespace

void feed(int64_t id, float x, float y, Phase phase, float width, float height) {
    size_t drawn = 0;
    plat::touch_buttons(drawn);
    std::lock_guard lk(g_mutex);
    g_w = width, g_h = height;
    if (phase == Phase::Down) {
        g_gamepad = false;
        if (drawn) {  // its buttons first, the rest of the left half is the stick, the rest of the right half nothing
            uint64_t b = plat::touch_hit(x / g_w, y / g_h, g_w / g_h);
            bool stick = !b && x < g_w * 0.5f;
            if (b || stick) g_fingers.push_back({id, x, y, x, y, stick, b});
        } else {
            bool stick = x < g_w * 0.5f;
            g_fingers.push_back({id, x, y, x, y, stick, stick ? 0 : zone_button(x / g_w, y / g_h)});
        }
    } else if (phase == Phase::Move) {
        for (auto& f : g_fingers)
            if (f.id == id) f.x = x, f.y = y;
    } else {
        g_fingers.erase(std::remove_if(g_fingers.begin(), g_fingers.end(), [&](const Finger& f) { return f.id == id; }),
                        g_fingers.end());
    }
}

void release_all() {
    std::lock_guard lk(g_mutex);
    g_fingers.clear();
}

void merge(plat::PadState& p) {
    size_t drawn = 0;
    plat::touch_buttons(drawn);
    std::lock_guard lk(g_mutex);
    float radius = std::max(1.f, g_h * 0.12f);
    for (const auto& f : g_fingers) {
        if (f.stick) {
            float dx = (f.x - f.x0) / radius, dy = (f.y - f.y0) / radius;
            p.lx = int32_t(std::clamp(dx, -1.f, 1.f) * plat::kStickMax);
            p.ly = int32_t(std::clamp(-dy, -1.f, 1.f) * plat::kStickMax);
            if (drawn && std::abs(dx) > 0.85f) p.buttons |= plat::kY;
        } else {
            p.buttons |= f.button;
        }
    }
}

void set_gamepad_active(bool active) {
    std::lock_guard lk(g_mutex);
    g_gamepad = active;
}

}  // namespace nx::touch

namespace nx::plat {

// The drawn layout's buttons (brighter while pressed) and the stick under the thumb, or a faint ring where it goes;
// nothing while a controller is in use or for games without a drawn layout.
size_t touch_overlay(OverlayShape* out, size_t max) {
    using touch::g_fingers, touch::g_w, touch::g_h;
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    if (!n) return 0;
    std::lock_guard lk(touch::g_mutex);
    if (touch::g_gamepad) return 0;
    const touch::Finger* stick = nullptr;
    uint64_t held = 0;
    for (const auto& f : g_fingers) {
        if (f.stick) stick = &f;
        held |= f.button;
    }
    for (size_t i = 0; i < n && k < max; i++) out[k++] = {b[i].x, b[i].y, b[i].r, held & b[i].button ? 0.5f : 0.2f};
    if (stick && k + 2 <= max) {
        float r = 0.12f, dx = (stick->x - stick->x0) / g_h, dy = (stick->y - stick->y0) / g_h;
        float len = std::hypot(dx, dy), s = len > r ? r / len : 1.f;
        out[k++] = {stick->x0 / g_w, stick->y0 / g_h, r, 0.14f};
        out[k++] = {(stick->x0 / g_h + dx * s) * g_h / g_w, stick->y0 / g_h + dy * s, 0.055f, 0.45f};
    } else if (k < max) {
        out[k++] = {0.15f, 0.72f, 0.12f, 0.08f};  // where the stick goes
    }
    return k;
}

}  // namespace nx::plat

#endif
