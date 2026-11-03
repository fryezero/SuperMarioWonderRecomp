// The GPU across the app's lifecycle (nvn_queue.cpp; the submits and presents it holds back are nvn_vk.cpp's). iOS ends
// an app whose Metal command buffers run in the background (MoltenVK takes the refusal as a lost device), so the
// platform suspends the GPU before the app leaves the foreground.
#pragma once

#include <cstdint>

namespace nx {

// Called on the platform's main thread when the app is about to lose the GPU (iOS: the scene will resign active or
// enters the background). Returns once no GPU work is in flight and none will start until gpu_resume(): submits wait
// (their work is kept), presents are skipped. Before the GPU exists it only sets the state. It waits for the threads
// that use the GPU, never for the GPU lock or a thread waiting for it, so it cannot deadlock with plat::create_window()
// (make_surface() calls it under that lock). Calling it again while suspended does nothing.
void gpu_suspend();
// The app is active again: waiting submits go on, presents resume.
void gpu_resume();
// Changes at every gpu_suspend and gpu_resume that changed the state, odd while suspended: a wait with a time limit on
// GPU work (a hung GPU) counts it again when the GPU was suspended meanwhile (nvn_queue.cpp).
uint64_t gpu_lifecycle_epoch();

}  // namespace nx
