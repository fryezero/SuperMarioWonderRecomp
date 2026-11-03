// What platform_darwin.cpp (the host-OS parts macOS and iOS share) gives the two Darwin front ends, platform_mac.cpp
// (SDL3) and platform_ios.mm (UIKit), beyond platform.h.
#pragma once

namespace nx::plat {

// SIGSEGV, SIGBUS, SIGILL and SIGFPE: a write watch fault is handed to write_watch.cpp; anything else logs the host
// pc, the guest registers and a guest backtrace, then the process dies of the signal. Call once, first thing.
void install_crash_handlers();

// Runs fn(arg) on a detached thread with a 64 MiB stack (recompiled calls nest deeply) at QOS_CLASS_USER_INTERACTIVE,
// for nx::boot. A guest longjmp with no setjmp frame or a host exception escaping fn is fatal; when fn returns the
// thread just ends, so fn decides whether the process ends with it.
void start_game_thread(void (*fn)(void*), void* arg);

// iOS only. log_memory (memguard_mac.cpp): one line with the footprint, the peak, what iOS still allows the app and
// the thermal state. thermal_state (platform_ios.mm): ProcessInfo's thermal state as a word ("nominal" to
// "critical"), the phone's warning that it is about to slow the CPU and GPU down.
void log_memory(const char* why);
const char* thermal_state();

}  // namespace nx::plat
