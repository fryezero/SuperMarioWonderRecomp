// NX_PHONE: 1 on phones (Android, iOS), 0 elsewhere. For defaults that follow the kind of device, not its OS or GPU
// driver: a wide touch screen (the present pass: bars and the touch overlay), a phone's memory (the 128 MB upload
// ring), clocks that drop when the load does (the 60/30 governor), and where the log goes. Workarounds for a driver
// stay keyed on it: __ANDROID__ for Adreno and the Android loader, __APPLE__ for MoltenVK (macOS and iOS alike).
#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define NX_PHONE 1
#else
#define NX_PHONE 0
#endif
