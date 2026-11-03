// iOS feasibility probe for the Wonder port: what the device gives a sideloaded app.
// Writes Documents/probe.txt (pull it with devicectl) and logs every line with NSLog.
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

static std::string g_out;
static long g_max_fps = -1;  // read on the main thread from the window scene's screen
static std::string g_os;

static void out(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_out += buf;
    g_out += "\n";
    fprintf(stderr, "PROBE %s\n", buf);
    fflush(stderr);
    // Keep a partial report on disk after every line, in case a later probe kills the process.
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    FILE* f = fopen([[docs stringByAppendingPathComponent:@"probe.txt"] fileSystemRepresentation], "w");
    if (f) {
        fwrite(g_out.data(), 1, g_out.size(), f);
        fclose(f);
    }
}

static std::string sysctl_str(const char* name) {
    char buf[256] = {};
    size_t n = sizeof buf;
    if (sysctlbyname(name, buf, &n, nullptr, 0) != 0) return "?";
    return buf;
}

static long long sysctl_int(const char* name) {
    long long v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, nullptr, 0) != 0) return -1;
    return n == 4 ? (long long)*(int*)&v : v;
}

static void probe_device() {
    out("== device");
    out("hw.machine %s  model %s", sysctl_str("hw.machine").c_str(), sysctl_str("hw.model").c_str());
    out("os %s", g_os.c_str());
    out("memsize %lld MiB  pagesize %lld", sysctl_int("hw.memsize") >> 20, sysctl_int("hw.pagesize"));
    out("cpus %lld  perf %lld  eff %lld", sysctl_int("hw.ncpu"), sysctl_int("hw.perflevel0.physicalcpu"),
        sysctl_int("hw.perflevel1.physicalcpu"));
    out("max fps %ld  thermal %ld  lowpower %d", g_max_fps,
        (long)[NSProcessInfo processInfo].thermalState, [NSProcessInfo processInfo].lowPowerModeEnabled);
}

static void probe_memory() {
    out("== memory");
    task_vm_info_data_t vi{};
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) {
        out("vm min_address %#llx  max_address %#llx", (unsigned long long)vi.min_address,
            (unsigned long long)vi.max_address);
        out("phys_footprint %llu MiB  limit_bytes_remaining %llu MiB", vi.phys_footprint >> 20,
            vi.limit_bytes_remaining >> 20);
    }
    size_t avail = os_proc_available_memory();
    out("os_proc_available_memory %zu MiB  => jetsam limit about %llu MiB", avail >> 20,
        (unsigned long long)((avail + vi.phys_footprint) >> 20));
}

// One 16 KiB page at each 1 GiB step: which parts of the address space can be mapped at fixed addresses.
static void probe_address_space() {
    out("== address space (fixed 16 KiB maps, 1 GiB steps)");
    std::string line;
    uint64_t first_fail_above_4g = 0, last_ok = 0;
    for (uint64_t a = 0x40000000ull; a < 0x8000000000ull; a += 0x40000000ull) {
        vm_address_t addr = (vm_address_t)a;
        kern_return_t kr = vm_allocate(mach_task_self(), &addr, 0x4000, VM_FLAGS_FIXED);
        bool ok = kr == KERN_SUCCESS;
        if (ok) {
            vm_deallocate(mach_task_self(), addr, 0x4000);
            last_ok = a;
        } else if (a >= 0x100000000ull && !first_fail_above_4g && kr != KERN_NO_SPACE) {
            first_fail_above_4g = a;
        }
        line += ok ? '+' : (kr == KERN_NO_SPACE ? 'o' : '.');
        if (((a / 0x40000000ull) % 64) == 63) {
            out("%#012llx %s", (unsigned long long)(a - 63 * 0x40000000ull), line.c_str());
            line.clear();
        }
    }
    out("legend: + mappable, o in use, . beyond limit.  highest mappable GiB step %#llx", (unsigned long long)last_ok);

    // The macOS layout of the runtime (runtime.h): can every range be reserved like plat::reserve does?
    struct R { const char* name; uint64_t addr, size; } rs[] = {
        {"modules", 0x300000000ull, 0x20000000ull}, {"heap", 0x400000000ull, 0x180000000ull},
        {"alias", 0x600000000ull, 0x400000000ull},  {"stack", 0xA00000000ull, 0x100000000ull},
        {"kernel", 0xB00000000ull, 0x100000000ull}, {"thunks", 0xE00000000ull, 0x100000ull},
    };
    out("== macOS layout reservations (mmap PROT_NONE MAP_NORESERVE at hint)");
    for (auto& r : rs) {
        void* p = mmap((void*)r.addr, r.size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        bool ok = p != MAP_FAILED && (uint64_t)p == r.addr;
        out("%-8s %#012llx +%#llx  %s (got %p)", r.name, (unsigned long long)r.addr, (unsigned long long)r.size,
            ok ? "OK" : "FAIL", p);
        if (p != MAP_FAILED) munmap(p, r.size);
    }
    // Largest single reservation anywhere.
    uint64_t lo = 0, hi = 0x2000000000ull;
    while (hi - lo > 0x10000000ull) {
        uint64_t mid = (lo + hi) / 2 & ~0x3FFFull;
        void* p = mmap(nullptr, mid, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) {
            munmap(p, mid);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    out("largest single anywhere reservation about %llu MiB", (unsigned long long)(lo >> 20));
}

static void probe_metal() {
    out("== metal");
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) {
        out("no Metal device");
        return;
    }
    out("name %s", d.name.UTF8String);
    for (int f = (int)MTLGPUFamilyApple7; f <= (int)MTLGPUFamilyApple7 + 6; f++)
        out("supportsFamily Apple%d %d", 7 + (f - (int)MTLGPUFamilyApple7), [d supportsFamily:(MTLGPUFamily)f]);
    out("supportsFamily Metal3 %d", [d supportsFamily:MTLGPUFamilyMetal3]);
    if (@available(iOS 16.4, *)) out("supportsBCTextureCompression %d", d.supportsBCTextureCompression);
    out("argumentBuffersSupport tier %ld", (long)d.argumentBuffersSupport);
    out("recommendedMaxWorkingSetSize %llu MiB", d.recommendedMaxWorkingSetSize >> 20);
    out("maxBufferLength %lu MiB", (unsigned long)(d.maxBufferLength >> 20));
    out("hasUnifiedMemory %d  maxThreadgroupMemoryLength %lu", d.hasUnifiedMemory,
        (unsigned long)d.maxThreadgroupMemoryLength);
}

static const char* fmt_support(VkPhysicalDevice pd, VkFormat f) {
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    bool s = p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    bool c = p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    bool d = p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    static char buf[8][16];
    static int i = 0;
    char* b = buf[i++ & 7];
    snprintf(b, 16, "%s%s%s", s ? "S" : "-", c ? "C" : "-", d ? "D" : "-");
    return b;
}

static void probe_vulkan() {
    out("== vulkan (MoltenVK static)");
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iext(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, iext.data());
    std::string s;
    for (auto& e : iext) s += std::string(e.extensionName) + " ";
    out("instance exts: %s", s.c_str());
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;
    // A statically linked MoltenVK has no loader, so no VK_KHR_portability_enumeration.
    const char* exts[] = {"VK_KHR_get_physical_device_properties2"};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    VkInstance inst;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS) {
        out("vkCreateInstance failed %d", r);
        return;
    }
    n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    for (auto pd : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        out("device %s api %u.%u.%u driver %#x", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
        auto& L = p.limits;
        out("limits: maxImageDim2D %u maxBoundSets %u perStage samplers %u sampledImg %u storageImg %u ubo %u ssbo %u"
            " resources %u",
            L.maxImageDimension2D, L.maxBoundDescriptorSets, L.maxPerStageDescriptorSamplers,
            L.maxPerStageDescriptorSampledImages, L.maxPerStageDescriptorStorageImages,
            L.maxPerStageDescriptorUniformBuffers, L.maxPerStageDescriptorStorageBuffers, L.maxPerStageResources);
        out("limits: set samplers %u sampledImg %u ubo %u ssbo %u  maxUboRange %u maxSsboRange %u pushConst %u"
            " maxVtxAttribs %u maxColorAtt %u",
            L.maxDescriptorSetSamplers, L.maxDescriptorSetSampledImages, L.maxDescriptorSetUniformBuffers,
            L.maxDescriptorSetStorageBuffers, L.maxUniformBufferRange, L.maxStorageBufferRange,
            L.maxPushConstantsSize, L.maxVertexInputAttributes, L.maxColorAttachments);
        out("limits: minUboAlign %llu minSsboAlign %llu minMemMapAlign %zu maxSamplerAniso %.0f maxViewports %u",
            (unsigned long long)L.minUniformBufferOffsetAlignment, (unsigned long long)L.minStorageBufferOffsetAlignment,
            L.minMemoryMapAlignment, L.maxSamplerAnisotropy, L.maxViewports);

        VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceDescriptorIndexingProperties dip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        dip.pNext = &emh;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dip;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        out("external_memory_host minImportedHostPointerAlignment %llu",
            (unsigned long long)emh.minImportedHostPointerAlignment);
        out("descriptor indexing: updateAfterBind samplers %u sampledImg %u ssbo %u",
            dip.maxDescriptorSetUpdateAfterBindSamplers, dip.maxDescriptorSetUpdateAfterBindSampledImages,
            dip.maxDescriptorSetUpdateAfterBindStorageBuffers);

        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceRobustness2FeaturesEXT rob2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        rob2.pNext = &eds3;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        v13.pNext = &rob2;
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.pNext = &v13;
        VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        v11.pNext = &v12;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &v11;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        auto& F = f2.features;
        out("features: BC %d ASTC_LDR %d ETC2 %d geometry %d tess %d logicOp %d depthClamp %d depthBiasClamp %d"
            " dualSrcBlend %d independentBlend %d multiViewport %d float64 %d int64 %d int16 %d fragStoresAtomics %d"
            " vtxStoresAtomics %d imageCubeArray %d sampleRateShading %d wideLines %d largePoints %d fillModeNonSolid %d"
            " shaderClipDistance %d shaderCullDistance %d samplerAniso %d occlusionQueryPrecise %d"
            " shaderStorageImageWriteWithoutFormat %d shaderStorageImageReadWithoutFormat %d",
            F.textureCompressionBC, F.textureCompressionASTC_LDR, F.textureCompressionETC2, F.geometryShader,
            F.tessellationShader, F.logicOp, F.depthClamp, F.depthBiasClamp, F.dualSrcBlend, F.independentBlend,
            F.multiViewport, F.shaderFloat64, F.shaderInt64, F.shaderInt16, F.fragmentStoresAndAtomics,
            F.vertexPipelineStoresAndAtomics, F.imageCubeArray, F.sampleRateShading, F.wideLines, F.largePoints,
            F.fillModeNonSolid, F.shaderClipDistance, F.shaderCullDistance, F.samplerAnisotropy,
            F.occlusionQueryPrecise, F.shaderStorageImageWriteWithoutFormat, F.shaderStorageImageReadWithoutFormat);
        out("v12: timelineSem %d descIndexing %d runtimeDescArray %d partiallyBound %d updAfterBindSampledImg %d"
            " bufferDeviceAddress %d scalarBlock %d float16 %d int8 %d samplerMirrorClampToEdge %d drawIndirectCount %d"
            " storageBuffer8BitAccess %d uniformBufferStandardLayout %d separateDepthStencilLayouts %d hostQueryReset %d",
            v12.timelineSemaphore, v12.descriptorIndexing, v12.runtimeDescriptorArray,
            v12.descriptorBindingPartiallyBound, v12.descriptorBindingSampledImageUpdateAfterBind,
            v12.bufferDeviceAddress, v12.scalarBlockLayout, v12.shaderFloat16, v12.shaderInt8,
            v12.samplerMirrorClampToEdge, v12.drawIndirectCount, v12.storageBuffer8BitAccess,
            v12.uniformBufferStandardLayout, v12.separateDepthStencilLayouts, v12.hostQueryReset);
        out("v13: dynamicRendering %d sync2 %d maintenance4 %d  v11: storageBuffer16BitAccess %d shaderDrawParameters %d"
            "  robust2: robustBuffer2 %d robustImage2 %d nullDescriptor %d",
            v13.dynamicRendering, v13.synchronization2, v13.maintenance4, v11.storageBuffer16BitAccess,
            v11.shaderDrawParameters, rob2.robustBufferAccess2, rob2.robustImageAccess2, rob2.nullDescriptor);
        out("eds3: polygonMode %d rasterSamples %d sampleMask %d alphaToCoverage %d logicOpEnable %d colorBlendEnable %d"
            " colorBlendEquation %d colorWriteMask %d depthClampEnable %d depthClipEnable %d",
            eds3.extendedDynamicState3PolygonMode, eds3.extendedDynamicState3RasterizationSamples,
            eds3.extendedDynamicState3SampleMask, eds3.extendedDynamicState3AlphaToCoverageEnable,
            eds3.extendedDynamicState3LogicOpEnable, eds3.extendedDynamicState3ColorBlendEnable,
            eds3.extendedDynamicState3ColorBlendEquation, eds3.extendedDynamicState3ColorWriteMask,
            eds3.extendedDynamicState3DepthClampEnable, eds3.extendedDynamicState3DepthClipEnable);

        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> dext(en);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, dext.data());
        s.clear();
        for (auto& e : dext) s += std::string(e.extensionName) + " ";
        out("device exts (%u): %s", en, s.c_str());

        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            out("heap %u: %llu MiB flags %#x", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
                mp.memoryHeaps[i].flags);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            out("memtype %u: heap %u flags %#x", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);

        struct FF { const char* n; VkFormat f; } fs[] = {
            {"BC1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK}, {"BC3", VK_FORMAT_BC3_UNORM_BLOCK},
            {"BC4", VK_FORMAT_BC4_UNORM_BLOCK},      {"BC5", VK_FORMAT_BC5_UNORM_BLOCK},
            {"BC6H", VK_FORMAT_BC6H_UFLOAT_BLOCK},   {"BC7", VK_FORMAT_BC7_UNORM_BLOCK},
            {"ASTC4x4", VK_FORMAT_ASTC_4x4_UNORM_BLOCK}, {"ASTC8x8", VK_FORMAT_ASTC_8x8_SRGB_BLOCK},
            {"D24S8", VK_FORMAT_D24_UNORM_S8_UINT},  {"D32S8", VK_FORMAT_D32_SFLOAT_S8_UINT},
            {"D16", VK_FORMAT_D16_UNORM},            {"D32", VK_FORMAT_D32_SFLOAT},
            {"X8D24", VK_FORMAT_X8_D24_UNORM_PACK32}, {"S8", VK_FORMAT_S8_UINT},
            {"R11G11B10", VK_FORMAT_B10G11R11_UFLOAT_PACK32}, {"RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT},
            {"RGB10A2", VK_FORMAT_A2B10G10R10_UNORM_PACK32}, {"RGBA8", VK_FORMAT_R8G8B8A8_UNORM},
            {"BGRA8srgb", VK_FORMAT_B8G8R8A8_SRGB},  {"R5G6B5", VK_FORMAT_R5G6B5_UNORM_PACK16},
            {"RGB9E5", VK_FORMAT_E5B9G9R9_UFLOAT_PACK32}, {"RGBA32F", VK_FORMAT_R32G32B32A32_SFLOAT},
        };
        s.clear();
        for (auto& f : fs) s += std::string(f.n) + "=" + fmt_support(pd, f.f) + " ";
        out("formats (S sampled, C color, D depth): %s", s.c_str());
    }
    vkDestroyInstance(inst, nullptr);
}

static void run_probes(UITextView* tv) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      probe_device();
      probe_memory();
      probe_address_space();
      probe_metal();
      probe_vulkan();
      out("== done");
      NSString* text = [NSString stringWithUTF8String:g_out.c_str()];
      dispatch_async(dispatch_get_main_queue(), ^{ tv.text = text; });
    });
}

// iOS 27 SDK apps must adopt the scene lifecycle (UIKit traps at launch otherwise).
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)opts {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    UIViewController* vc = [UIViewController new];
    UITextView* tv = [[UITextView alloc] initWithFrame:vc.view.bounds];
    tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    tv.editable = NO;
    tv.font = [UIFont monospacedSystemFontOfSize:9 weight:UIFontWeightRegular];
    tv.text = @"probing...";
    [vc.view addSubview:tv];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    g_max_fps = (long)((UIWindowScene*)scene).screen.maximumFramesPerSecond;
    g_os = [[[UIDevice currentDevice] systemVersion] UTF8String];
    run_probes(tv);
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
