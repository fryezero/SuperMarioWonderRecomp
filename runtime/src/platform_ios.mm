// iOS host (iPhone, UIKit): the app and its scene, a CAMetalLayer for MoltenVK (VK_EXT_metal_surface; MoltenVK is
// linked in, iOS has no Vulkan loader), GameController pads + touch, a RemoteIO audio unit, the display's vsync and
// main. Memory, threads, files and the crash handler are platform_darwin.cpp's, shared with macOS.
//
// UIKit owns the main thread: main() reserves the guest's address space and hands the thread to UIApplicationMain.
// The view's first layout with a size publishes its layer and starts the game (nx::boot) on its own thread. The game's
// threads wait for that layer and never ask the main thread for anything: create_window() runs under the GPU lock,
// and the main thread itself waits for the GPU in gpu_suspend(). A CADisplayLink at 60 Hz samples the controller into
// a snapshot read_pad() copies, then ticks the game's vsync (vsync_source).
//
// Data: the app's Documents folder (shared through Finder and Files): analysis/{main,sdk}.bin, extract/romfs/, spirv/,
// fonts/, user/ (saves), pipelines_*.bin, nx.log. Settings: NX_<KEY> environment variables, as on the Mac; the NX_*
// lines of Documents/config.txt are put into the environment at launch.
#if !__has_feature(objc_arc)
#error "platform_ios.mm is written for ARC (-fobjc-arc)"
#endif

#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>  // with prototypes: vkGetInstanceProcAddr is MoltenVK's own, linked statically

#include <nx/game.h>
#include <nx/runtime.h>

#include "gpu_lifecycle.h"
#include "platform.h"
#include "platform_darwin.h"
#include "touch_controls.h"

namespace nx {
void boot();
}

namespace nx::plat {


namespace {

// The layer the game draws to, published once by the main thread (the view's first layout) and kept for good.
std::mutex g_layer_lock;
std::condition_variable g_layer_cv;
void* g_layer = nullptr;  // CAMetalLayer*, retained

std::atomic<bool> g_minimized{true};  // the scene is not foreground-active (nor before it first is)
std::atomic<uint32_t> g_pixel_w{0}, g_pixel_h{0};  // the layer's drawable

std::mutex g_pad_lock;
PadState g_pad;          // the controller, sampled on the main thread at each display refresh
bool g_gamepad = false;  // main thread: the touch overlay is hidden for a controller in use

std::atomic<void (*)(uint64_t)> g_vsync_tick{nullptr};

void publish_layer(void* layer) {
    {
        std::lock_guard lk(g_layer_lock);
        g_layer = layer;
    }
    g_layer_cv.notify_all();
}

// The drawable's scale: 2 by default. The present pass scales the game's 720p picture up to the screen anyway, and
// the screen's native 3x would make every swapchain image 2.25 times as big for nothing. NX_IOS_SCALE changes it,
// between 1 and the screen's native scale.
CGFloat draw_scale(UIScreen* screen) {
    static const double wanted = [] {
        std::string s = setting("ios_scale");
        double v = s.empty() ? 2.0 : std::strtod(s.c_str(), nullptr);
        return v > 0 ? v : 2.0;
    }();
    return CGFloat(std::clamp(wanted, 1.0, screen ? double(screen.nativeScale) : 3.0));
}

// The current controller by position, as on the Mac. GCExtendedGamepad names the face buttons Xbox-style (A at the
// bottom, B on the right); Nintendo's A is the right one, B the bottom one.
void sample_pad() {
    PadState p;
    GCController* c = GCController.current ?: GCController.controllers.firstObject;
    if (GCExtendedGamepad* g = c.extendedGamepad) {
        auto press = [&](GCControllerButtonInput* b, uint64_t bit) {
            if (b.pressed) p.buttons |= bit;  // a pad without that button has nil: not pressed
        };
        press(g.buttonB, kA);
        press(g.buttonA, kB);
        press(g.buttonY, kX);
        press(g.buttonX, kY);
        press(g.leftShoulder, kL);
        press(g.rightShoulder, kR);
        press(g.buttonMenu, kPlus);
        press(g.buttonOptions, kMinus);
        press(g.leftThumbstickButton, kStickL);
        press(g.rightThumbstickButton, kStickR);
        press(g.dpad.left, kLeft);
        press(g.dpad.up, kUp);
        press(g.dpad.right, kRight);
        press(g.dpad.down, kDown);
        // The Mac's thresholds in SDL's units (12000 and 7849 of 32767): a trigger past 37% is ZL / ZR, a stick within
        // 24% of the centre is centred.
        if (g.leftTrigger.value > 0.37f) p.buttons |= kZL;
        if (g.rightTrigger.value > 0.37f) p.buttons |= kZR;
        auto axis = [](float v) {
            int i = int(std::lround(v * float(kStickMax)));
            return i > -7849 && i < 7849 ? 0 : std::clamp(i, -kStickMax, kStickMax);
        };
        p.lx = axis(g.leftThumbstick.xAxis.value);
        p.ly = axis(g.leftThumbstick.yAxis.value);  // up is positive, as on Horizon
        p.rx = axis(g.rightThumbstick.xAxis.value);
        p.ry = axis(g.rightThumbstick.yAxis.value);
    }
    // A controller in use hides the touch controls; the next finger down brings them back (touch::feed).
    if ((p.buttons || p.lx || p.ly || p.rx || p.ry) && !g_gamepad) {
        g_gamepad = true;
        touch::set_gamepad_active(true);
    }
    std::lock_guard lk(g_pad_lock);
    g_pad = p;
}

}  // namespace

}  // namespace nx::plat

// ---- UIKit: the display link, the view, its controller, the scene and the app ---------------------------------

@interface WonderTicker : NSObject
@end

@implementation WonderTicker

- (void)refresh:(CADisplayLink*)link {
    nx::plat::sample_pad();  // first, so the game woken by this vsync reads the freshest input
    // link.timestamp is on CACurrentMediaTime()'s clock, mach_absolute_time in seconds; monotonic_ns() reads
    // CLOCK_UPTIME_RAW, the same ticks in ns (timebase 125/3 on Apple silicon; the two agree to within 0.1 us).
    if (auto tick = nx::plat::g_vsync_tick.load(std::memory_order_acquire)) tick(uint64_t(link.timestamp * 1e9));
}

@end

@interface WonderView : UIView
@end

@implementation WonderView {
    BOOL _started;
}

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.multipleTouchEnabled = YES;
        self.opaque = YES;
        self.backgroundColor = UIColor.blackColor;
    }
    return self;
}

// MoltenVK sizes the swapchain by the layer's bounds times its contentsScale, which follows contentScaleFactor.
- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat scale = nx::plat::draw_scale(self.window.windowScene.screen);
    if (self.contentScaleFactor != scale) self.contentScaleFactor = scale;
    CGSize size = self.bounds.size;
    uint32_t w = uint32_t(size.width * scale), h = uint32_t(size.height * scale);
    if (w && h) ((CAMetalLayer*)self.layer).drawableSize = CGSizeMake(w, h);  // CAMetalLayer refuses an empty one
    if (w != nx::plat::g_pixel_w || h != nx::plat::g_pixel_h)
        nx::log("platform: drawable %ux%u (%.0fx%.0f points at scale %.2f)", w, h, size.width, size.height, scale);
    nx::plat::g_pixel_w = w, nx::plat::g_pixel_h = h;
    if (w && h && !_started) {
        _started = YES;
        [self start];
    }
}

- (void)start {
    std::string root = nx::plat::data_root();
    if (access((root + "/analysis/main.bin").c_str(), R_OK) != 0) {
        nx::log("platform: no analysis/main.bin in %s: the game data is missing", root.c_str());
        [self showMissingData];
        return;
    }
    nx::plat::publish_layer((__bridge_retained void*)self.layer);  // for the process's lifetime (MoltenVK's surface)
    nx::plat::start_game_thread([](void*) { nx::boot(); nx::plat::exit_process(0); }, nullptr);
}

// Without its data the game would end at once; say what is missing instead.
- (void)showMissingData {
    UILabel* label = [[UILabel alloc] initWithFrame:CGRectInset(self.bounds, 48, 32)];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    label.numberOfLines = 0;
    label.textAlignment = NSTextAlignmentCenter;
    label.textColor = UIColor.whiteColor;
    label.text = [NSString stringWithFormat:@"%s: the game data is missing.\n\nCopy analysis/, extract/romfs/, "
                                            @"spirv/ and fonts/ into this app's Documents folder (Finder: the "
                                            @"iPhone's Files tab), then open the app again.",
                                            nx::game::kInfo.title];
    [self addSubview:label];
}

- (void)feedTouches:(NSSet<UITouch*>*)touches phase:(nx::touch::Phase)phase {
    CGSize size = self.bounds.size;
    for (UITouch* t in touches) {
        CGPoint at = [t locationInView:self];
        nx::touch::feed(int64_t(intptr_t((__bridge void*)t)), float(at.x), float(at.y), phase, float(size.width),
                        float(size.height));
    }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    nx::plat::g_gamepad = false;  // touch::feed shows the overlay again; a controller's next input hides it
    [self feedTouches:touches phase:nx::touch::Phase::Down];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self feedTouches:touches phase:nx::touch::Phase::Move];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self feedTouches:touches phase:nx::touch::Phase::Up];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self feedTouches:touches phase:nx::touch::Phase::Up];
}

@end

@interface WonderViewController : UIViewController
@end

@implementation WonderViewController

- (void)loadView {
    self.view = [[WonderView alloc] initWithFrame:CGRectZero];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

// A swipe from an edge (the stick thumb sliding off the bottom, a press on a corner button) reaches the game; only a
// second swipe goes to the system.
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    return UIRectEdgeAll;
}

@end

@interface WonderSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation WonderSceneDelegate

- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    UIWindowScene* ws = (UIWindowScene*)scene;
    // One view controller for the process: should iOS discard the scene and connect a new one, the new window shows
    // the same view, so the layer the game's surface was made from stays on screen (window_generation stays 1).
    static WonderViewController* vc = [WonderViewController new];
    self.window = [[UIWindow alloc] initWithWindowScene:ws];
    self.window.backgroundColor = UIColor.blackColor;
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    UIScreen* screen = ws.screen;
    nx::log("platform: screen %.0fx%.0f points, scale %.2f (native %.2f), up to %ld Hz", screen.bounds.size.width,
            screen.bounds.size.height, screen.scale, screen.nativeScale, long(screen.maximumFramesPerSecond));
    static bool linked = false;
    if (!linked) {
        linked = true;
        CADisplayLink* link = [CADisplayLink displayLinkWithTarget:[WonderTicker new] selector:@selector(refresh:)];
        link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
        [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];  // also while UIKit tracks a gesture
    }
}

- (void)sceneDidBecomeActive:(UIScene*)scene {
    nx::log("platform: active");
    nx::gpu_resume();
    nx::plat::audio_pause(false);
    nx::plat::g_minimized = false;
    // A controller's input does not hold off auto-lock: without this the screen dims mid-course (Android:
    // FLAG_KEEP_SCREEN_ON).
    UIApplication.sharedApplication.idleTimerDisabled = YES;
}

// Both come on the way to the background (resign active alone: Control Center, a call, the app switcher).
- (void)sceneWillResignActive:(UIScene*)scene {
    [self suspend:"inactive"];
}

- (void)sceneDidEnterBackground:(UIScene*)scene {
    [self suspend:"background"];
}

- (void)suspend:(const char*)why {
    nx::log("platform: %s", why);
    nx::plat::g_minimized = true;
    nx::touch::release_all();  // a finger whose cancel never came must not hold the stick on return
    nx::plat::audio_pause(true);
    nx::gpu_suspend();  // returns once the GPU is idle: iOS ends an app whose command buffers run in the background
}

@end

@interface WonderAppDelegate : UIResponder <UIApplicationDelegate>
@end

namespace nx::plat {

const char* thermal_state() {
    switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateNominal: return "nominal";
        case NSProcessInfoThermalStateFair: return "fair";
        case NSProcessInfoThermalStateSerious: return "serious";
        case NSProcessInfoThermalStateCritical: return "critical";
    }
    return "?";
}

}  // namespace nx::plat

@implementation WonderAppDelegate

- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    NSOperationQueue* main = NSOperationQueue.mainQueue;
    [nc addObserverForName:GCControllerDidConnectNotification
                    object:nil
                     queue:main
                usingBlock:^(NSNotification* n) {
                  GCController* c = n.object;
                  nx::log("platform: controller connected: %s%s", c.vendorName.UTF8String ?: "?",
                          c.extendedGamepad ? "" : " (not a full gamepad: not used)");
                }];
    [nc addObserverForName:GCControllerDidDisconnectNotification
                    object:nil
                     queue:main
                usingBlock:^(NSNotification* n) {
                  GCController* c = n.object;
                  nx::log("platform: controller disconnected: %s", c.vendorName.UTF8String ?: "?");
                  nx::plat::g_gamepad = false;  // the touch controls come back; another pad's input hides them again
                  nx::touch::set_gamepad_active(false);
                }];
    [nc addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                    object:nil
                     queue:main
                usingBlock:^(NSNotification*) { nx::log("platform: thermal state %s", nx::plat::thermal_state()); }];
    nx::log("platform: thermal state %s", nx::plat::thermal_state());
    [nc addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                    object:nil
                     queue:main
                usingBlock:^(NSNotification*) { nx::plat::log_memory("warning from iOS"); }];
    return YES;
}

@end

namespace nx::plat {

// ---- data and settings ----------------------------------------------------------------------------------------

std::string data_root() {
    static const std::string root = [] {
        @autoreleasepool {
            NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
            return std::string(dirs.count ? dirs[0].fileSystemRepresentation : ".");
        }
    }();
    return root;
}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}

// ---- window ---------------------------------------------------------------------------------------------------

bool headless() { return false; }

void* create_window() {
    std::unique_lock lk(g_layer_lock);
    g_layer_cv.wait(lk, [] { return g_layer != nullptr; });
    return g_layer;  // the Vulkan surface is made from the CAMetalLayer, from any thread
}

void release_window(void*) {}
uint64_t window_generation() { return 1; }  // the layer outlives the background
bool window_minimized() { return g_minimized; }

void window_size(uint32_t& w, uint32_t& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }  // the view fills the landscape screen
float default_render_scale() { return 1.0f; }

bool vsync_source(void (*tick)(uint64_t vsync_ns)) {
    g_vsync_tick.store(tick, std::memory_order_release);  // WonderTicker calls it from the next refresh on
    return true;
}

// ---- input: the controller snapshot, then touch -----------------------------------------------------------------

void read_pad(PadState& p) {
    {
        std::lock_guard lk(g_pad_lock);
        p.buttons |= g_pad.buttons;
        if (g_pad.lx) p.lx = g_pad.lx;
        if (g_pad.ly) p.ly = g_pad.ly;
        if (g_pad.rx) p.rx = g_pad.rx;
        if (g_pad.ry) p.ry = g_pad.ry;
    }
    touch::merge(p);
    input_script(p);
}

// ---- audio: a RemoteIO unit, s16 stereo 48 kHz, its render callback pulling from the queue of submitted buffers ----

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
std::mutex g_audio;  // the queue
std::deque<Pending> g_pending;
AudioDone g_done = nullptr;
std::vector<void*> g_finished;  // the render thread's (render calls never overlap), reserved: no allocation there

std::mutex g_unit_lock;  // the unit and whether it plays
AudioUnit g_unit = nullptr;
bool g_paused = false, g_interrupted = false;
std::atomic<bool> g_running{false};  // written under g_unit_lock, read by audio_submit under g_audio

OSStatus render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList* io) {
    auto* dst = static_cast<uint8_t*>(io->mBuffers[0].mData);
    size_t want = io->mBuffers[0].mDataByteSize;  // interleaved: one buffer of frames x 4 bytes
    g_finished.clear();
    {
        std::lock_guard lk(g_audio);
        while (want && !g_pending.empty()) {
            Pending& p = g_pending.front();
            size_t n = std::min(want, p.size - p.pos);
            std::memcpy(dst, p.data + p.pos, n);
            dst += n;
            want -= n;
            p.pos += n;
            if (p.pos == p.size) {
                g_finished.push_back(p.ctx);
                g_pending.pop_front();
            }
        }
    }
    if (want) std::memset(dst, 0, want);  // underrun: silence
    for (void* c : g_finished) g_done(c);
    return noErr;
}

bool activate_session() {
    NSError* err = nil;
    if ([AVAudioSession.sharedInstance setActive:YES error:&err]) return true;
    nx::log("audio: session not activated: %s", err.localizedDescription.UTF8String ?: "?");
    return false;
}

// What is still queued when the unit stops is dropped: played after a stay in the background it would be stale, and
// the queue, full by then, would keep the sound 60 ms late for the rest of the session (measured on the phone).
void drop_pending() {
    std::vector<void*> dropped;
    {
        std::lock_guard lk(g_audio);
        for (const Pending& p : g_pending) dropped.push_back(p.ctx);
        g_pending.clear();
    }
    for (void* c : dropped) g_done(c);
}

// Under g_unit_lock: the unit plays unless the app is in the background or another app's sound took over. While it
// is stopped audio_submit reports no device, so the game paces its sound itself instead of filling the queue.
void update_unit() {
    bool play = !g_paused && !g_interrupted;
    if (!g_unit || play == g_running) return;
    OSStatus s = play ? AudioOutputUnitStart(g_unit) : AudioOutputUnitStop(g_unit);
    if (s == noErr)
        g_running = play;
    else
        nx::log("audio: %s failed (%d)", play ? "AudioOutputUnitStart" : "AudioOutputUnitStop", int(s));
    if (!g_running) drop_pending();
}

// A call, an alarm, Siri: the unit stops when the interruption begins and plays again when it ends, once the session
// is ours again.
void interrupted(bool began, const char* what) {
    nx::log("audio: %s", what);
    bool paused;
    {
        std::lock_guard lk(g_unit_lock);
        paused = g_paused;
    }
    // In the background the session is taken back when the app returns (audio_pause), not over other apps' sound now.
    bool active = !began && !paused && activate_session();
    std::lock_guard lk(g_unit_lock);
    g_interrupted = !active;
    update_unit();
}

void observe_interruptions() {
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    NSOperationQueue* main = NSOperationQueue.mainQueue;
    if (@available(iOS 27.0, *)) {
        // iOS 27 tells the two ends apart: the session made inactive, later a recommendation to resume. The app never
        // deactivates its session itself, so inactive always means interrupted.
        [nc addObserverForName:AVAudioSessionDidBecomeInactiveNotification
                        object:nil
                         queue:main
                    usingBlock:^(NSNotification*) { interrupted(true, "session made inactive"); }];
        [nc addObserverForName:AVAudioSessionResumptionRecommendationNotification
                        object:nil
                         queue:main
                    usingBlock:^(NSNotification*) { interrupted(false, "session free to resume"); }];
        return;
    }
#if __IPHONE_OS_VERSION_MIN_REQUIRED < 270000  // older systems: only where the deployment target lets them run it
    [nc addObserverForName:AVAudioSessionInterruptionNotification
                    object:nil
                     queue:main
                usingBlock:^(NSNotification* n) {
                  bool began = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] ==
                               AVAudioSessionInterruptionTypeBegan;
                  interrupted(began, began ? "interruption began" : "interruption ended");
                }];
#endif
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    if (std::getenv("NX_MUTE")) return false;  // silent: the callers pace themselves with a timer
    @autoreleasepool {
        AVAudioSession* s = AVAudioSession.sharedInstance;
        NSError* err = nil;
        // Playback: the silent switch does not mute the game, and other apps' sound stops while it plays.
        if (![s setCategory:AVAudioSessionCategoryPlayback error:&err])
            nx::log("audio: category not set: %s", err.localizedDescription.UTF8String ?: "?");
        [s setPreferredSampleRate:48000 error:nil];    // the game's rate: no resampling where the route allows it
        [s setPreferredIOBufferDuration:0.01 error:nil];  // about 480 frames per render call
        activate_session();

        AudioComponentDescription d{kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple};
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        AudioUnit unit = nullptr;
        if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) {
            nx::log("audio: no RemoteIO unit");
            return false;
        }
        AudioStreamBasicDescription f{};
        f.mSampleRate = 48000;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = 2;
        f.mBitsPerChannel = 16;
        f.mBytesPerFrame = f.mBytesPerPacket = 4;
        AURenderCallbackStruct cb{render, nullptr};
        OSStatus st =
            AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
        if (st == noErr)
            st = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                      sizeof cb);
        if (st == noErr) st = AudioUnitInitialize(unit);
        if (st != noErr) {
            nx::log("audio: RemoteIO setup failed (%d)", int(st));
            AudioComponentInstanceDispose(unit);
            return false;
        }
        g_finished.reserve(64);
        observe_interruptions();
        nx::log("audio: RemoteIO, 48 kHz s16 stereo; the route runs at %.0f Hz with %.1f ms buffers", s.sampleRate,
                s.IOBufferDuration * 1e3);
        std::lock_guard lk(g_unit_lock);
        g_unit = unit;
        update_unit();
    }
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_unit) return false;  // set by audio_open, on this same thread
    std::lock_guard lk(g_audio);
    // Checked under the queue lock: update_unit clears g_running before drop_pending takes it, so nothing queued
    // after the check outlives a stop.
    if (!g_running.load(std::memory_order_acquire)) return false;
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

void audio_pause(bool paused) {
    bool open;
    {
        std::lock_guard lk(g_unit_lock);
        open = g_unit != nullptr;
    }
    // Back in the foreground the session may still be someone else's: an interruption that began while the app was
    // suspended never reports its end.
    bool active = !paused && open && activate_session();
    std::lock_guard lk(g_unit_lock);
    g_paused = paused;
    if (active) g_interrupted = false;
    update_unit();
}

// ---- Vulkan (MoltenVK, linked in) -------------------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() { return reinterpret_cast<void*>(&vkGetInstanceProcAddr); }

const char* vulkan_surface_extension() { return VK_EXT_METAL_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto gipa_fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa);
    auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        gipa_fn(static_cast<VkInstance>(instance), "vkCreateMetalSurfaceEXT"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    ci.pLayer = (__bridge const CAMetalLayer*)native_window;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- entry ----------------------------------------------------------------------------------------------------

namespace {

// config.txt's NX_* lines become environment variables (the NVN layer reads its switches with getenv), without
// overriding one already set (Xcode's scheme, devicectl --environment-variables).
void load_config() {
    std::ifstream f(nx::plat::data_root() + "/config.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto eq = line.find('=');
        if (line.rfind("NX_", 0) == 0 && eq != std::string::npos)
            setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 0);
    }
    // iOS defaults. Handheld: the game renders for 1280x720, the lighter GPU load (svc_am.cpp). MoltenVK: errors and
    // warnings, without its default informational messages (a GPU hang's cause comes from its error log,
    // docs/nvn/DEBUG.md #21). The present pass (bars on the 19.5:9 screen, the touch controls) needs nothing here: it
    // is on by default on phones (phone.h).
    setenv("NX_HANDHELD", "1", 0);
    setenv("MVK_CONFIG_LOG_LEVEL", "2", 0);
}

std::string machine() {
    char buf[64] = {};
    size_t n = sizeof buf - 1;
    sysctlbyname("hw.machine", buf, &n, nullptr, 0);
    return buf;
}

}  // namespace

int main(int argc, char* argv[]) {
    // The guest's regions first, before UIKit and Metal take address space in the window they must fit in (memory.cpp).
    nx::memory_init();
    nx::plat::install_crash_handlers();
    load_config();
    nx::plat::start_memory_guard();
    @autoreleasepool {
        nx::log("%s (Switch recomp) on iOS: %s, %s, data at %s", nx::game::kInfo.title, machine().c_str(),
                NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String, nx::plat::data_root().c_str());
        return UIApplicationMain(argc, argv, nil, @"WonderAppDelegate");
    }
}
