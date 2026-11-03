# iOS

Wonder 1.2.1 runs on the iPhone 18 Pro Max (iPhone19,3, A20 Pro, iOS 27.2) at 60 fps. The recompiled code is the same
as on the other hosts. iOS gets a UIKit front end (`runtime/src/platform_ios.mm`), and shares the POSIX and Mach half
with macOS (`runtime/src/platform_darwin.cpp`). Graphics are NVN over Vulkan on MoltenVK, linked in statically.

| | macOS | iOS |
|---|---|---|
| App | SDL3 window, main loop | UIKit, UIScene lifecycle (required by the iOS 27 SDK: an app without it aborts at launch) |
| Memory | `platform_darwin.cpp` (mmap at fixed addresses) | the same, reserved in `main()` before UIKit starts |
| Window | SDL Metal view | full-screen `CAMetalLayer` view, drawable at 2x (`NX_IOS_SCALE`) |
| Vsync | 60 Hz timer | `CADisplayLink` at 60/60/60, which also paces the presents; MoltenVK offers only FIFO on iOS |
| Input | SDL gamepad + keyboard | GameController pads (by position, like the Mac) + touch (`touch_controls.cpp`, shared with Android) |
| Audio | SDL3 stream | AVAudioSession (playback) + RemoteIO, 48 kHz s16 stereo, 10 ms buffers |
| Vulkan | Homebrew loader + MoltenVK ICD | static `libMoltenVK.a` 1.4.2 (`vkGetInstanceProcAddr` linked in, no loader) |
| Background | n/a | `gpu_suspend()` at resign-active (iOS ends an app that runs GPU work in the background), audio stopped |

## What the device allows, and why the guest lives at 0x7F_E000_0000

Measured with `tools/ios_probe` (a small app that reports the address space, memory limit, Metal and MoltenVK
features):

- The app is signed by a free personal team. Xcode refuses `com.apple.developer.kernel.extended-virtual-addressing`
  for those teams, but `com.apple.developer.kernel.increased-memory-limit` signs (`ios/Wonder.entitlements`).
- Without entitlements, iOS gives the app about 3.4 GB and 6 GiB of free address space at 0x70_0000_0000. Everything
  from 0x1_4000_0000 to 0x70_0000_0000 is taken by the system.
- With increased-memory-limit, 0x70_0000_0000..0x80_0000_0000 (64 GiB) is free, and iOS reports about 6 GB at launch
  (about 6.5 GB still available at a 1.5 GB footprint in play).
- macOS 27 has the same window as its only large free space below 0x80_0000_0000, and puts a 24 GiB heap range at a
  random place inside it at every launch.

So on Apple hosts the modules sit at the top of the window (`NX_BASE` 0x7F_E000_0000, compiled into the generated
code). `memory_init()` probes the window in 256 MiB steps and places the heap, alias, stack and kernel regions below
the modules, top down, where the host left room (`GuestLayout` in `runtime/include/nx/runtime.h`). The guest learns
every region from svcGetInfo. The ASLR region (svcGetInfo 12/13) spans only the guest's regions, and everything in it
that is not ours is reported inaccessible.

## Build, install, data

Requirements: Xcode 27 signed in to the Apple ID of the signing team, the phone paired with Developer Mode on, and the
generated code and data as for macOS (README: recompile, translate the shaders, build the button font), plus
MoltenVK's iOS release in `private/tools/moltenvk`:

```sh
gh release download v1.4.2 -R KhronosGroup/MoltenVK -p MoltenVK-ios.tar -D private/tools/moltenvk
tar xf private/tools/moltenvk/MoltenVK-ios.tar -C private/tools/moltenvk
```

The scripts read the signing team and the phone from the environment or from `private/ios.env` (`NX_IOS_TEAM=...` and
`NX_IOS_DEVICE=<UDID>` lines; `tools/ios_env.sh` lists them). The bundle id defaults to `com.wonderrecomp.<team>`.

```sh
tools/build_ios.sh --install       # Xcode project in private/wonder/build-ios, about 6 min
tools/wonder_push_ios.sh           # analysis/, spirv/, fonts/, extract/romfs/ -> Documents
tools/build_ios.sh --launch --console   # start it; stdout and MoltenVK's messages stay attached
tools/wonder_push_ios.sh --pull-log     # Documents/nx.log -> private/wonder/ios_nx.log
```

The first push copies about 4 GB over USB in under a minute. Later pushes skip unchanged files. Reinstalling the app
keeps its Documents, but deleting the app deletes the data with it.

A free team's profile lasts 7 days: after that the app no longer opens until `tools/build_ios.sh --install` signs it
again. A free team can also have only 3 apps of its own installed at a time.

Switches for one run go through devicectl's environment prefix, e.g.
`DEVICECTL_CHILD_NX_INPUT_SCRIPT="25:A:0.5;..." tools/build_ios.sh --launch`. For every run, put `NX_...=value`
lines in `Documents/config.txt` (Files app or Finder).

## Controls

A Bluetooth controller (DualSense, Xbox, Switch Pro, Joy-Con pair) is mapped by position, as on the Mac: the east
button is A, the south button B. Touch controls as on Android: the left half is a stick that appears where the thumb
lands, the right side has A (jump), B, Y (dash) and X, and the top has ZR, R, L, + and -. They hide while a
controller is in use.

## Performance on the iPhone 18 Pro Max

Handheld mode (720p), present pass to a 1912x880 drawable, display-locked FIFO:

- Title and menus: 60 fps, replay about 9 ms a frame.
- Course 1-1, a 30-minute soak walking and jumping: median 60.0 fps in every 5-minute window, 97.2% of the 110,399
  present intervals within 14-19 ms, thermal state nominal (briefly fair), memory 3.3-3.5 GB with 4.6 GB to spare.
- The frames that used to miss were the first encounter of new graphics: each new pipeline is a Metal shader compile
  (about 40 ms cold). The first run of course 1-1 compiled 1,754 pipelines in 20 s, up to 8.3 s at once when the
  course starts.

### Pipeline capture and warm-up

Every pipeline the replay creates is recorded device-independently in `Documents/pipeline_capture.bin` (SPIR-V of
each module once, the whole create info, the layout's definition; `nvn_pipeline_capture.cpp`). At the next start one
thread at user-initiated QoS rebuilds the records in the order they were first needed, into its own pipeline cache,
and keeps the pipelines: when the replay misses its own cache it finds the same create info there by hash and takes it,
or waits for the one being built (`nvn_pipeline_warm.cpp`). Identical create infos reached from different NVN states
share one pipeline (the replay used to make about three of each). A capture made on the Mac is valid on the phone:
`tools/wonder_push_ios.sh` copies `private/wonder/pipeline_capture.bin`, and the phone appends what it sees beyond it.

First start of a fresh install (no MoltenVK cache, no system Metal cache), the course-1-1 script, iPhone 18 Pro Max:

| | below 57 fps | lowest second | replay time on pipelines |
|---|---|---|---|
| no capture | 28 of 247 s | 6.3 fps | 20,244 ms |
| capture, warm-up at utility QoS (Metal compiles at 110 ms each) | 7 of 259 s | 13.5 fps | 6,863 ms |
| capture, warm-up at user-initiated QoS | 0 of 266 s | 57.2 fps | 75 ms (1,704 of 1,705 misses handed over) |

The warm-up took 34 s on that cold start (639 pipelines), while the title screen kept 57-60 fps, and about 0.1 s on
later starts. Several threads do not help on the phone: Metal's compiles contend (4 threads: 96 ms a pipeline).
`NX_PIPELINE_WARM=0` turns it off, `=<n>` sets the threads (docs/nvn/SWITCHES.md).

What stays: graphics that no capture has seen yet still compile the first time (an area of a course the capture never
reached: up to 3.4 s once on a cold start). Each run adds them to the capture, so it happens once.
