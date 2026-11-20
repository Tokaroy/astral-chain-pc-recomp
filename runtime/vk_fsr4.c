// AMD FSR 4: AMD FSR Upscaling 4 (the ML upscaler) as the scene's temporal upscaler, behind AC_UPSCALING=fsr4
// (astralchain.ini upscaling = fsr4), through AMD's FSR API: ffxCreateContext with an upscale description chained to the
// Vulkan back end's, ffxDispatch with the frame's resources, ffxDestroyContext once no frame in flight uses the
// context.
//
// The library is AMD's own on every platform: amd_fidelityfx_loader_vk.dll, which finds amd_fidelityfx_upscaler_vk.dll
// next to it, AMD's signed DLLs of the FSR SDK 2.4.0 (FSR 4.1.2; tools/fsr4_sdk.py pins them), loaded at run time from
// the folder of the executable:
// - Windows: the loader DLL itself. The release ships the two DLLs in bin/ and the installer copies them.
// - Linux: AMD publishes no Linux library, so the same two DLLs run through ffx-vk-linux (libffx_vk_linux.so, MIT,
//   github.com/DuIslingr/ffx-vk-linux: a PE loader with the Windows services and the calling-convention bridges the
//   DLLs need, no Wine), which exports the FSR API's entry points under their own names. Its ffxvkLoad is given the
//   folder of the DLLs. The three files are checked for before anything is loaded (files_there): a missing one is
//   named in the log and to the menu (vk_fsr4_missing), FSR 4 is off and vk_dlaa.c has FSR 3 run in its place.
// AC_FSR4_LIB names another file to load (the loader DLL on Windows, the bridge on Linux). AMD's FSR 4.1 runs on the
// Radeon RX 6000 series and newer and on the Ryzen processors with RDNA 2 or newer graphics; the menu offers it on
// those only (vk_fsr4_offered). No AMD header is compiled in: the few structures used are declared here from the SDK's
// C ABI (ffx_api.h, ffx_api_types.h, vk/ffx_api_vk.h, upscalers/include/ffx_upscale.h of SDK 2.4.0), as vk_xess.c does
// for Intel's.
//
// FSR 4's model: AMD's library picks it from the GPU family it runs on (the full network on the Radeon RX 7000 and
// 9000 series, a version of it for the RX 6000, FSR 4 Lite, a smaller network, on its integrated GPUs) and makes FSR
// 3.1 for a family it does not list. The SDK 2.4.0 DLL lists no family for the integrated RDNA 3 GPUs before the
// Radeon 890M (a Ryzen Z1 Extreme's Radeon 780M, family GC 11.0.1, got 3.1.6). On Linux, where the bridge answers the
// library's question about the family, an AMD integrated GPU that got FSR 3.1 is asked again for FSR 4 Lite
// (ffxvkFsr4Model: the bridge then names the Radeon 890M's family; the library's Vulkan queries still see the real
// GPU): the Z1 Extreme runs FSR 4.1.2 Lite that way, validation layer silent. AC_FSR4_MODEL=lite asks for Lite at
// once on any GPU, =own never asks for it.
//
// vk_dlaa.c does everything around FSR 4 as for FSR 3 (vk_fsr3.c) and XeSS: it finds the scene, jitters its draws, makes
// the motion vectors and calls vk_fsr4_evaluate where the upscaler runs, then copies the result where the game reads it.
// The inputs follow FSR 3's conventions, which are the FSR API's: jitter in pixels (the picture's shift, x right, y
// down), motion vectors in render pixels pointing from this frame's pixel to its last-frame position (scale 1, 1),
// depth near 0 to far 1 (not inverted, not infinite), linear scene light with auto exposure, or the tonemapped
// composite with an exposure of 1.
//
// What AMD's library is given beyond that, from the SDK's documentation
// (Kits/FidelityFX/docs/techniques/super-resolution-ml.md) and its Vulkan back end (backend/vk/ffx_vk.cpp, MIT, read for
// what it does with a resource description):
// - the API version (ffxCreateContextDescUpscaleVersion), which the documentation requires since SDK 2.1;
// - the non-linear colour flag, and sRGB at the dispatch, when the colour is the tonemapped composite, which is
//   sRGB-encoded (vk_hdr.c): FSR 4 expects linear colour otherwise;
// - the stencil usage beside the depth usage on a depth image with stencil: the back end makes its view in the format
//   the usage gives, and without it a D32_SFLOAT view of the game's D32_SFLOAT_S8_UINT depth, which is not valid (the
//   validation layer reported it at every frame);
// - and the context it made is checked. The loader makes its newest upscaler that takes the GPU, so on a GPU FSR 4
//   does not run on it makes FSR 3.1 without an error (an Arc 140T: provider 3.1.6, under the name FSR 4 in the menu
//   and the log). A context that is not FSR 4 is destroyed and FSR 4 is off for the run: the menu says it cannot run
//   here and vk_dlaa.c has FSR 3 run in its place. AC_FSR4_ANY_VERSION=1 (test) keeps such a context, to exercise
//   this file on another GPU.
//
// Threads: the contexts are made and dispatched on the replay thread, but for the first, which the boot thread makes
// before any frame (vk_fsr4_warm). One context per scene size, output size and colour
// kind, at most CTX_MAX (handheld and docked differ, so do HDR output on and off and each game resolution chosen in the
// menu): the least recently used goes for a new one, and all go once FSR 4 has not run for a while (vk_fsr4_trim; AMD
// gives a working set of 81 MB at 1920x1080 and 318 MB at 3840x2160). ffxDestroyContext does not wait for the GPU and
// a frame in flight may still use the context, so a context that goes is destroyed once the timeline has passed the
// submission being recorded when it went (vk_fsr4_sweep, at each present), as vk_dlaa.c does for DLSS's features.
#include "vk_fsr4.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- the FSR API's ABI (SDK 2.4.0)
typedef void *FfxContext;
typedef uint32_t FfxReturn;   // 0 FFX_API_RETURN_OK
typedef uint64_t FfxType;
typedef struct FfxHeader {
    FfxType type;
    struct FfxHeader *next;
} FfxHeader;
typedef void (*FfxMessageFn)(uint32_t type, const wchar_t *message);

#define FFX_EFFECT_UPSCALE 0x00010000u
#define FFX_BACKEND_VK 0x01000000u
#define FFX_TYPE_CREATE_UPSCALE ((FfxType)(FFX_EFFECT_UPSCALE | 0x00))
#define FFX_TYPE_DISPATCH_UPSCALE ((FfxType)(FFX_EFFECT_UPSCALE | 0x01))
#define FFX_TYPE_BACKEND_VK ((FfxType)(FFX_BACKEND_VK | 0x03))
#define FFX_TYPE_QUERY_PROVIDER_VERSION ((FfxType)6)
#define FFX_TYPE_CREATE_UPSCALE_VERSION ((FfxType)(FFX_EFFECT_UPSCALE | 0x0b))
#define FFX_UPSCALER_VERSION ((4u << 22) | (1u << 12) | 2u)   // FFX_UPSCALER_VERSION of SDK 2.4.0: 4.1.2

enum {   // FfxApiCreateContextUpscaleFlags
    FFX_UP_HDR = 1 << 0,
    FFX_UP_AUTO_EXPOSURE = 1 << 5,
    FFX_UP_NON_LINEAR = 1 << 8,
};
enum { FFX_DISPATCH_NON_LINEAR_SRGB = 1 << 1 };   // FfxApiDispatchFsrUpscaleFlags
enum {   // FfxApiSurfaceFormat (the ones used here)
    FFX_FMT_UNKNOWN = 0,
    FFX_FMT_R32G32B32A32_FLOAT = 3,
    FFX_FMT_R16G16B16A16_FLOAT = 4,
    FFX_FMT_R32_UINT = 8,
    FFX_FMT_R8G8B8A8_UNORM = 10,
    FFX_FMT_R8G8B8A8_SRGB = 12,
    FFX_FMT_B8G8R8A8_UNORM = 14,
    FFX_FMT_B8G8R8A8_SRGB = 15,
    FFX_FMT_R11G11B10_FLOAT = 16,
    FFX_FMT_R10G10B10A2_UNORM = 17,
    FFX_FMT_R16G16_FLOAT = 18,
    FFX_FMT_R16_UNORM = 23,
    FFX_FMT_R32_FLOAT = 28,
};
enum { FFX_RES_TEXTURE2D = 2 };
enum {
    FFX_USAGE_READ_ONLY = 0,
    FFX_USAGE_UAV = 1 << 1,
    FFX_USAGE_DEPTHTARGET = 1 << 2,
    FFX_USAGE_STENCILTARGET = 1 << 5,
};
enum { FFX_STATE_UNORDERED_ACCESS = 1 << 1, FFX_STATE_COMPUTE_READ = 1 << 2 };   // GENERAL, SHADER_READ_ONLY_OPTIMAL

typedef struct {
    uint32_t type, format, width, height, depth, mip_count, flags, usage;
} FfxResourceDesc;
typedef struct {
    void *resource;   // the VkImage
    FfxResourceDesc desc;
    uint32_t state;
} FfxResource;
typedef struct { uint32_t width, height; } FfxDim;
typedef struct { float x, y; } FfxXY;

typedef struct {
    FfxHeader header;
    uint32_t flags;
    FfxDim max_render, max_upscale;
    FfxMessageFn message;
} FfxCreateUpscale;
typedef struct {
    FfxHeader header;
    uint32_t version;
} FfxCreateUpscaleVersion;
typedef struct {
    FfxHeader header;
    VkDevice device;
    VkPhysicalDevice phys;
    PFN_vkGetDeviceProcAddr get_device_proc;
} FfxBackendVk;
typedef struct {
    FfxHeader header;
    void *command_list;
    FfxResource color, depth, motion, exposure, reactive, transparency, output;
    FfxXY jitter, mv_scale;
    FfxDim render_size, upscale_size;
    bool sharpen;
    float sharpness, frame_time_ms, pre_exposure;
    bool reset;
    float near_z, far_z, fov_y, meters_per_unit;
    uint32_t flags;
} FfxDispatchUpscale;
typedef struct {
    FfxHeader header;
    uint64_t version_id;
    const char *version_name;
} FfxQueryProviderVersion;
_Static_assert(sizeof(FfxResourceDesc) == 32, "FfxApiResourceDescription layout");
_Static_assert(sizeof(FfxResource) == 48, "FfxApiResource layout");
_Static_assert(sizeof(FfxCreateUpscale) == 48, "ffxCreateContextDescUpscale layout");
_Static_assert(sizeof(FfxCreateUpscaleVersion) == 24, "ffxCreateContextDescUpscaleVersion layout");
_Static_assert(sizeof(FfxBackendVk) == 40, "ffxCreateBackendVKDesc layout");
// header, command list, 7 resources, jitter and scale, two sizes, sharpen (padded), 3 floats, reset (padded), 4 floats,
// flags
_Static_assert(sizeof(FfxDispatchUpscale) == 16 + 8 + 7 * 48 + 16 + 16 + 4 + 12 + 4 + 16 + 4,
               "ffxDispatchDescUpscale layout");

typedef FfxReturn (*PFN_Create)(FfxContext *, FfxHeader *, const void *);
typedef FfxReturn (*PFN_Destroy)(FfxContext *, const void *);
typedef FfxReturn (*PFN_Query)(FfxContext *, FfxHeader *);
typedef FfxReturn (*PFN_Dispatch)(FfxContext *, const FfxHeader *);

// ffx-vk-linux's own entry points (ffx_vk_linux.h)
typedef int (*PFN_BridgeLoad)(const char *directory);   // 0: AMD's DLLs are loaded from there
typedef const char *(*PFN_BridgeError)(void);
typedef int (*PFN_BridgeModel)(const char *model);      // "lite": FSR 4 Lite for the contexts made from now on

#define CTX_MAX 4
#define DEAD_MAX 8

static struct {
    void *lib;
    PFN_Create create;
    PFN_Destroy destroy;
    PFN_Query query;
    PFN_Dispatch dispatch;
    PFN_BridgeModel model;   // Linux: the bridge's ffxvkFsr4Model
    int tried, loaded, failed;
    int lite;                // FSR 4 Lite was asked for (the header's paragraph on the model)
    char path[PLAT_PATH_MAX];
    const char *missing;     // the file FSR 4 needs that is not there (files_there), NULL when none is
    struct {
        FfxContext ctx;
        uint32_t w, h, ow, oh, flags;
        uint64_t used;   // F.clock when it was last asked for
    } ctx[CTX_MAX];
    struct {
        FfxContext ctx;
        uint64_t after;   // the timeline value that must be done before it is destroyed
    } dead[DEAD_MAX];
    uint64_t clock, evals;
    VkImage exposure;   // 1x1 R32F holding 1: the exposure of the tonemapped composite
    VmaAllocation exposure_mem;
} F;

int vk_fsr4_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = !strcmp(cfg_upscaling(), "fsr4");
    return wanted;
}

int vk_fsr4_tried(void) { return F.tried; }

int vk_fsr4_available(void) { return F.loaded && !F.failed; }

#define AMD_LOADER "amd_fidelityfx_loader_vk.dll"
#define AMD_UPSCALER "amd_fidelityfx_upscaler_vk.dll"
#ifdef _WIN32
#define FSR4_LIB AMD_LOADER
#define FSR4_NEEDS "AMD's " AMD_LOADER " and " AMD_UPSCALER " (FSR SDK 2.4.0)"
#else
#define FSR4_LIB "libffx_vk_linux.so"   // the bridge: it loads AMD's two DLLs from the folder it is given
#define FSR4_NEEDS FSR4_LIB " (ffx-vk-linux) and AMD's " AMD_LOADER " and " AMD_UPSCALER " (FSR SDK 2.4.0)"
#endif

// The executable's folder, its last separator included. 0 when the executable's path is unknown.
static int exe_dir(char *dir, size_t size) {
    int len = plat_exe_path(dir, size - 40);   // room for a file name of this file's
    char *cut = len ? plat_last_sep(dir) : NULL;
    if (!cut) return 0;
    cut[1] = 0;
    return 1;
}

// The library's path: AC_FSR4_LIB, else FSR4_LIB next to the executable. 0 when the executable's path is unknown.
static int lib_path(char *path, size_t size) {
    const char *own = cfg_get("AC_FSR4_LIB");
    if (own && *own) {
        snprintf(path, size, "%s", own);
        return 1;
    }
    if (!exe_dir(path, size)) return 0;
    strcat(path, FSR4_LIB);
    return 1;
}

// Every file FSR 4 loads is there: the library and, on Linux, AMD's two DLLs next to the executable, which the bridge
// loads from that folder (on Windows the loader DLL finds the upscaler DLL beside itself). F.missing names the first
// that is not.
static int files_there(void) {
    static char lib_name[80];
    char path[PLAT_PATH_MAX];
    F.missing = NULL;
    if (!lib_path(path, sizeof path)) return 0;
    if (!plat_file_exists(path)) {
        const char *cut = plat_last_sep(path);
        snprintf(lib_name, sizeof lib_name, "%s", cut ? cut + 1 : path);
        F.missing = lib_name;
        return 0;
    }
#ifndef _WIN32
    static const char *const dlls[] = {AMD_LOADER, AMD_UPSCALER};
    for (int i = 0; i < 2; i++) {
        if (!exe_dir(path, sizeof path)) return 0;
        strcat(path, dlls[i]);
        if (!plat_file_exists(path)) {
            F.missing = dlls[i];
            return 0;
        }
    }
#endif
    return 1;
}

const char *vk_fsr4_missing(void) { return F.missing; }

static int lib_load(void) {
    if (F.tried) return F.loaded;
    F.tried = 1;
    char dir[PLAT_PATH_MAX] = "";
    if (!lib_path(F.path, sizeof F.path) || !exe_dir(dir, sizeof dir)) return 0;
    if (!files_there()) {
        if (F.missing)
            rt_log("fsr4: %s is missing: FSR 4 needs " FSR4_NEEDS " in %s: FSR 4 off, FSR 3 runs in its place",
                   F.missing, dir);
        return 0;
    }
    F.lib = plat_lib_open(F.path);
    if (!F.lib) {
        const char *why = plat_lib_error();
        rt_log("fsr4: cannot load %s%s%s: FSR 4 off, FSR 3 runs in its place", F.path, *why ? ": " : "", why);
        return 0;
    }
    F.create = (PFN_Create)plat_lib_sym(F.lib, "ffxCreateContext");
    F.destroy = (PFN_Destroy)plat_lib_sym(F.lib, "ffxDestroyContext");
    F.query = (PFN_Query)plat_lib_sym(F.lib, "ffxQuery");
    F.dispatch = (PFN_Dispatch)plat_lib_sym(F.lib, "ffxDispatch");
    if (!F.create || !F.destroy || !F.dispatch) {
        rt_log("fsr4: %s lacks the FSR API's entry points: FSR 4 off, FSR 3 runs in its place", F.path);
        return 0;
    }
#ifndef _WIN32
    // the bridge loads AMD's DLLs now, so a DLL it cannot run is known here and not at the first frame
    PFN_BridgeLoad load = (PFN_BridgeLoad)plat_lib_sym(F.lib, "ffxvkLoad");
    PFN_BridgeError why = (PFN_BridgeError)plat_lib_sym(F.lib, "ffxvkError");
    F.model = (PFN_BridgeModel)plat_lib_sym(F.lib, "ffxvkFsr4Model");
    if (!load || load(dir) != 0) {
        rt_log("fsr4: %s could not load AMD's DLLs from %s (%s): FSR 4 off, FSR 3 runs in its place", F.path, dir,
               load && why ? why() : "it has no ffxvkLoad");
        return 0;
    }
    const char *model = cfg_get("AC_FSR4_MODEL");
    if (model && !strcmp(model, "lite") && F.model && F.model("lite") == 0) {
        F.lite = 1;
        rt_log("fsr4: FSR 4 Lite asked for (AC_FSR4_MODEL=lite)");
    }
#endif
    rt_log("fsr4: %s loaded", F.path);
    return F.loaded = 1;
}

// Whether a GPU AMD's library made FSR 3.1 for is asked again for FSR 4 Lite (the header's paragraph on the model):
// on Linux, an AMD integrated GPU, unless AC_FSR4_MODEL=own.
static int lite_may(void) {
#ifdef _WIN32
    return 0;
#else
    const char *model = cfg_get("AC_FSR4_MODEL");
    const VkCtx *c = vk_ctx();
    return F.model && !F.lite && !(model && !strcmp(model, "own")) && c->props.vendorID == 0x1002 &&
           c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
#endif
}

// Whether the menu offers FSR 4 on this GPU: an AMD Radeon of the RDNA 2 architecture or newer (AMD's FSR 4.1 runs
// on the Radeon RX 6000 series and newer and on the Ryzen processors with RDNA 2 or newer graphics; told apart by
// VK_KHR_ray_query, which AMD's drivers offer from RDNA 2 on, the first Radeons with ray accelerators), with the
// library's files there. Whether AMD's library takes this GPU is known only once it runs (an integrated GPU AMD does
// not list gets FSR 3.1 from it, and FSR 3 runs), so a choice that fails says so in the menu.
static int offered;

static int has_ray_query(void) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(vk_ctx()->phys, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(vk_ctx()->phys, NULL, &n, e);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(e[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    free(e);
    return found;
}

void vk_fsr4_init(void) {
    int amd = vk_ctx()->props.vendorID == 0x1002, rdna2 = amd && has_ray_query();
    int lib = files_there();
    offered = rdna2 && lib;
    if (vk_fsr4_wanted() || amd)
        rt_log("fsr4: %s offered (AMD GPU %d, RDNA 2 or newer %d, library %d)", offered ? "is" : "is not", amd, rdna2, lib);
    if (vk_fsr4_wanted()) lib_load();
}

int vk_fsr4_offered(void) { return offered; }

static int fail(const char *what, FfxReturn r) {
    rt_log("fsr4: %s failed (%u): FSR 4 off for this run", what, (unsigned)r);
    F.failed = 1;
    return 0;
}

// The runtime's messages are wide strings (UTF-16 on Windows, UTF-32 elsewhere): logged as their ASCII part.
static void on_message(uint32_t type, const wchar_t *message) {
    char s[512];
    size_t n = 0;
    for (; message && message[n] && n < sizeof s - 1; n++) s[n] = message[n] < 128 ? (char)message[n] : '?';
    s[n] = 0;
    rt_log("fsr4: %s: %s", type == 0 ? "error" : "warning", s);
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return FFX_FMT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_FMT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_FMT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_FMT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_FMT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_FMT_B8G8R8A8_SRGB;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_FMT_R11G11B10_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_FMT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_FMT_R16G16_FLOAT;
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT: return FFX_FMT_R16_UNORM;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_FMT_R32_FLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return FFX_FMT_R32_UINT;
    default: return FFX_FMT_UNKNOWN;
    }
}

// The inputs are read as sampled images (COMPUTE_READ: AMD's documentation asks for them in SHADER_READ_ONLY_OPTIMAL),
// the output written as storage.
static int resource(FfxResource *r, const Fsr4Image *im, uint32_t usage, uint32_t state, const char *what) {
    static int warned;
    if (im->level || im->layer) {
        if (!warned++)
            rt_log("fsr4: the %s is level %u layer %u of its image; the FSR API takes level 0 layer 0", what, im->level,
                   im->layer);
        return 0;
    }
    uint32_t fmt = surface_format(im->format);
    if (fmt == FFX_FMT_UNKNOWN) {
        if (!warned++) rt_log("fsr4: the %s's format %d has no FSR API format", what, (int)im->format);
        return 0;
    }
    *r = (FfxResource){.resource = (void *)im->image,
                       .desc = {.type = FFX_RES_TEXTURE2D, .format = fmt, .width = im->width, .height = im->height,
                                .depth = 1, .mip_count = 1, .usage = usage},
                       .state = state};
    return 1;
}

// The exposure image, made and cleared to 1 in cb at the first dispatch; it then stays in SHADER_READ_ONLY_OPTIMAL.
static int exposure_image(VkCommandBuffer cb) {
    if (F.exposure) return 1;
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R32_SFLOAT, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &F.exposure, &F.exposure_mem, NULL) != VK_SUCCESS) {
        F.exposure = VK_NULL_HANDLE;
        return 0;
    }
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = F.exposure,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
    VkClearColorValue one = {.float32 = {1.0f, 1.0f, 1.0f, 1.0f}};
    vkCmdClearColorImage(cb, F.exposure, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cb, &di);
    return 1;
}

static VkImageAspectFlags aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// The depth's usage: AMD's back end makes its view of the image in the depth format the usage and the surface format
// give together (D32_SFLOAT_S8_UINT for R32_FLOAT with depth and stencil, D32_SFLOAT with depth alone), so an image
// with stencil says so, as AMD's ffxApiGetImageResourceDescriptionVK does.
static uint32_t depth_usage(VkFormat f) {
    return FFX_USAGE_DEPTHTARGET | (aspect(f) & VK_IMAGE_ASPECT_STENCIL_BIT ? FFX_USAGE_STENCILTARGET : 0);
}

// The inputs live in GENERAL; FSR 4 reads them in SHADER_READ_ONLY_OPTIMAL (their COMPUTE_READ state), so they move
// there for the dispatch and back after it. to_read 0: back to GENERAL.
static void inputs_layout(VkCommandBuffer cb, const Fsr4Eval *e, int to_read) {
    const Fsr4Image *in[3] = {&e->color, &e->depth, &e->mv};
    VkImageMemoryBarrier2 b[3];
    for (int i = 0; i < 3; i++)
        b[i] = (VkImageMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = to_read ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = to_read ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask = to_read ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = to_read ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = in[i]->image,
            .subresourceRange = {aspect(in[i]->format), 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3,
                           .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Contexts that went while submissions that dispatch them may still be on the GPU (the header's last paragraph).
static void sweep(int all) {
    uint64_t done = vk_sync_fence_done();
    for (int i = 0; i < DEAD_MAX; i++)
        if (F.dead[i].ctx && (all || F.dead[i].after <= done)) F.destroy(&F.dead[i].ctx, NULL), F.dead[i].ctx = NULL;
}

void vk_fsr4_sweep(void) {
    if (F.loaded) sweep(0);
}

static void retire(FfxContext ctx) {
    for (int i = 0; i < DEAD_MAX; i++)
        if (!F.dead[i].ctx) {
            F.dead[i].ctx = ctx, F.dead[i].after = vk_sync_issued() + 1;   // the submission being recorded included
            return;
        }
    // no room (DEAD_MAX contexts gone within a few frames): the slow way, wait for the GPU and destroy everything
    vk_queue_lock();   // vkDeviceWaitIdle needs every queue externally synchronised; other threads submit too
    vkDeviceWaitIdle(vk_ctx()->device);
    vk_queue_unlock();
    sweep(1);
    F.destroy(&ctx, NULL);
}

void vk_fsr4_trim(void) {
    int n = 0;
    for (int i = 0; i < CTX_MAX; i++)
        if (F.ctx[i].ctx) retire(F.ctx[i].ctx), F.ctx[i].ctx = NULL, n++;
    if (n) rt_log("fsr4: %d context%s released", n, n == 1 ? "" : "s");
}

// The context for this scene size, output size and colour kind, made at its first use; with every slot taken, in the
// place of the least recently used one.
static FfxContext *context(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t flags) {
    int free_slot = -1, lru = 0;
    F.clock++;
    for (int i = 0; i < CTX_MAX; i++) {
        if (F.ctx[i].ctx && F.ctx[i].w == w && F.ctx[i].h == h && F.ctx[i].ow == ow && F.ctx[i].oh == oh &&
            F.ctx[i].flags == flags) {
            F.ctx[i].used = F.clock;
            return &F.ctx[i].ctx;
        }
        if (!F.ctx[i].ctx && free_slot < 0) free_slot = i;
        if (F.ctx[i].used < F.ctx[lru].used) lru = i;
    }
    if (free_slot < 0) {
        rt_log("fsr4: the context %ux%u to %ux%u goes for another size", F.ctx[lru].w, F.ctx[lru].h, F.ctx[lru].ow,
               F.ctx[lru].oh);
        retire(F.ctx[lru].ctx), F.ctx[lru].ctx = NULL;
        free_slot = lru;
    }
    const VkCtx *c = vk_ctx();
    FfxCreateUpscaleVersion ver = {.header = {FFX_TYPE_CREATE_UPSCALE_VERSION, NULL}, .version = FFX_UPSCALER_VERSION};
    FfxBackendVk be = {.header = {FFX_TYPE_BACKEND_VK, &ver.header},
                       .device = c->device, .phys = c->phys, .get_device_proc = vkGetDeviceProcAddr};
    FfxCreateUpscale cu = {.header = {FFX_TYPE_CREATE_UPSCALE, &be.header}, .flags = flags,
                           .max_render = {w, h}, .max_upscale = {ow, oh}, .message = on_message};
    static int any = -1;
    if (any < 0) any = rt_knob("FSR4_ANY_VERSION", "0");   // AC_FSR4_ANY_VERSION=1: keep an FSR 3.1 context (test)
    uint64_t t0 = plat_now_ns();
    FfxContext ctx = NULL;
    const char *name;
    int old;   // AMD's loader made its newest upscaler that takes this GPU: FSR 3.1 where FSR 4 does not run (the header)
    for (;;) {
        FfxReturn r = F.create(&ctx, &cu.header, NULL);
        if (r != 0 || !ctx) return fail("context creation", r), NULL;
        name = "?";
        FfxQueryProviderVersion q = {.header = {FFX_TYPE_QUERY_PROVIDER_VERSION, NULL}};
        if (F.query && F.query(&ctx, &q.header) == 0 && q.version_name) name = q.version_name;
        old = name[0] >= '0' && name[0] <= '9' && atoi(name) < 4;
        if (!old || any || !lite_may()) break;
        // an AMD integrated GPU the library lists no FSR 4 model for: once more, asking for FSR 4 Lite (the header)
        rt_log("fsr4: the library made its FSR %s upscaler for this GPU: asking it for FSR 4 Lite, the model AMD gives "
               "its integrated GPUs", name);
        F.destroy(&ctx, NULL);   // nothing was dispatched with it
        ctx = NULL;
        F.lite = 1;              // asked once, whatever comes of it
        if (F.model("lite") != 0) return fail("the choice of FSR 4 Lite", 0), NULL;
    }
    if (old) {
        if (!any) {
            F.destroy(&ctx, NULL);   // nothing was dispatched with it
            rt_log("fsr4: the library made its FSR %s upscaler for this GPU, not FSR 4: FSR 4 off for this run", name);
            F.failed = 1;
            return NULL;
        }
        static int said;
        if (!said++) rt_log("fsr4: the library made its FSR %s upscaler, kept for AC_FSR4_ANY_VERSION", name);
    }
    F.ctx[free_slot].ctx = ctx;
    F.ctx[free_slot].w = w, F.ctx[free_slot].h = h, F.ctx[free_slot].ow = ow, F.ctx[free_slot].oh = oh;
    F.ctx[free_slot].flags = flags, F.ctx[free_slot].used = F.clock;
    rt_log("fsr4: context %ux%u to %ux%u, %s colour, provider %s%s, made in %.1f ms", w, h, ow, oh,
           flags & FFX_UP_HDR ? "linear" : "LDR", name, F.lite ? " (FSR 4 Lite asked for)" : "",
           (plat_now_ns() - t0) / 1e6);
    return &F.ctx[free_slot].ctx;
}

// The context's flags for a colour kind: linear scene light, or the tonemapped composite, which is sRGB-encoded (the
// header).
static uint32_t context_flags(int hdr) { return hdr ? FFX_UP_HDR | FFX_UP_AUTO_EXPOSURE : FFX_UP_NON_LINEAR; }

// AMD's library compiles its upscaler's shaders when it makes a context (FSR 4's network is 30 compute shaders): 0.2
// to 1 s for 1920x1080 and 2.6 s for 3840x2160 on a Ryzen Z1 Extreme, twice where FSR 4 Lite is asked for after an
// FSR 3.1 context. At the first frame with a 3D scene, which comes right after a load, that was a stall the player
// saw, and it left a cutscene's audio ahead of its picture. So the context the game's sizes give is made here, while
// the game starts; another size (handheld, HDR output switched) still makes its own at its first frame.
void vk_fsr4_warm(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int hdr) {
    if (!vk_fsr4_available() || !w || !h) return;
    context(w, h, ow ? ow : w, oh ? oh : h, context_flags(hdr));
}

int vk_fsr4_evaluate(VkCommandBuffer cb, const Fsr4Eval *e) {
    if (!vk_fsr4_available()) return 0;
    uint32_t ow = e->ow ? e->ow : e->w, oh = e->oh ? e->oh : e->h;
    FfxDispatchUpscale d = {.header = {FFX_TYPE_DISPATCH_UPSCALE, NULL}, .command_list = (void *)cb};
    if (!resource(&d.color, &e->color, FFX_USAGE_READ_ONLY, FFX_STATE_COMPUTE_READ, "scene colour") ||
        !resource(&d.depth, &e->depth, depth_usage(e->depth.format), FFX_STATE_COMPUTE_READ, "scene depth") ||
        !resource(&d.motion, &e->mv, FFX_USAGE_READ_ONLY, FFX_STATE_COMPUTE_READ, "motion vectors") ||
        !resource(&d.output, &e->out, FFX_USAGE_UAV, FFX_STATE_UNORDERED_ACCESS, "output"))
        return fail("resource description", 0);
    if (!exposure_image(cb)) return fail("exposure image", 0);
    Fsr4Image ex = {.image = F.exposure, .format = VK_FORMAT_R32_SFLOAT, .width = 1, .height = 1};
    resource(&d.exposure, &ex, FFX_USAGE_READ_ONLY, FFX_STATE_COMPUTE_READ, "exposure");
    uint32_t flags = context_flags(e->hdr);
    if (flags & FFX_UP_NON_LINEAR) d.flags = FFX_DISPATCH_NON_LINEAR_SRGB;
    FfxContext *ctx = context(e->w, e->h, ow, oh, flags);
    if (!ctx) return 0;
    d.jitter = (FfxXY){e->jx, e->jy};
    d.mv_scale = (FfxXY){1.0f, 1.0f};
    d.render_size = (FfxDim){e->w, e->h};
    d.upscale_size = (FfxDim){ow, oh};
    d.sharpen = false;   // the present's RCAS sharpens, as for the other upscalers
    d.frame_time_ms = e->dt_ms;
    d.pre_exposure = 1.0f;
    d.reset = e->reset != 0;
    d.near_z = e->near_z, d.far_z = e->far_z, d.fov_y = e->fov_y;
    d.meters_per_unit = 1.0f;
    // The barriers here (and the exposure image's clear) go through the recorder thread (vk_rec.h) like the replay's own
    // commands; the library records into cb itself, so the recorder must have made them first, as for XeSS
    // (vk_xess.c). Without the wait both threads recorded into cb at once, which the validation layer reported on an
    // Arc 140T (the recorder thread is off on AMD GPUs). The barrier after is queued behind the library's commands.
    inputs_layout(cb, e, 1);
    rec_drain();
    FfxReturn r = F.dispatch(ctx, &d.header);
    inputs_layout(cb, e, 0);   // also after a refusal: the images stay in GENERAL for FSR 3
    if (r != 0) return fail("dispatch", r);
    if (!F.evals++) rt_log("fsr4: first dispatch recorded (%ux%u to %ux%u)", e->w, e->h, ow, oh);
    return 1;
}

void vk_fsr4_release(void) {
    if (F.loaded) sweep(1);
    for (int i = 0; i < CTX_MAX; i++)
        if (F.ctx[i].ctx) F.destroy(&F.ctx[i].ctx, NULL), F.ctx[i].ctx = NULL;
    if (F.exposure) vmaDestroyImage(vk_ctx()->vma, F.exposure, F.exposure_mem), F.exposure = VK_NULL_HANDLE;
}
