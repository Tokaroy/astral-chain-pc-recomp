// AMD FSR 4: AMD FSR Upscaling 4 (the ML upscaler) as the scene's temporal upscaler, behind AC_UPSCALING=fsr4
// (astralchain.ini upscaling = fsr4), through AMD's FSR API: ffxCreateContext with an upscale description chained to the
// Vulkan back end's, ffxDispatch with the frame's resources, ffxDestroyContext at exit.
//
// The library is loaded at run time from the folder of the executable:
// - Windows: amd_fidelityfx_loader_vk.dll, which finds amd_fidelityfx_upscaler_vk.dll next to it: AMD's signed DLLs of
//   the FSR SDK 2.4.0 (FSR 4.1.2), which the release ships in bin/ and the installer copies (tools/fsr4_sdk.py pins
//   them). AMD's FSR 4.1 runs on the Radeon RX 6000 series and newer and on the Ryzen processors with RDNA 2 or newer
//   graphics; the menu offers it on those only (vk_fsr4_offered).
// - elsewhere: amd_fidelityfx_upscaler_vk.so, not shipped (AMD publishes no Linux library): a library with the same
//   entry points that the player puts there.
// AC_FSR4_LIB names another file. No AMD header is compiled in: the few structures used are declared here from the SDK's
// C ABI (ffx_api.h, ffx_api_types.h, vk/ffx_api_vk.h, upscalers/include/ffx_upscale.h of SDK 2.4.0), as vk_xess.c does
// for Intel's.
//
// vk_dlaa.c does everything around FSR 4 as for FSR 3 (vk_fsr3.c) and XeSS: it finds the scene, jitters its draws, makes
// the motion vectors and calls vk_fsr4_evaluate where the upscaler runs, then copies the result where the game reads it.
// The inputs follow FSR 3's conventions, which are the FSR API's: jitter in pixels (the picture's shift, x right, y
// down), motion vectors in render pixels pointing from this frame's pixel to its last-frame position (scale 1, 1),
// depth near 0 to far 1 (not inverted, not infinite), linear scene light with auto exposure, or the tonemapped
// composite with an exposure of 1.
//
// Threads: the contexts are made and dispatched on the replay thread, and destroyed at shutdown once the device is idle.
// One context per scene size, output size and colour kind (at most CTX_MAX: handheld and docked differ); none is
// destroyed before exit, since one may still be in use by a frame in flight.
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
#define FFX_TYPE_BACKEND_VK_OLD ((FfxType)0x3)   // FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED
#define FFX_TYPE_QUERY_PROVIDER_VERSION ((FfxType)6)

enum {   // FfxApiCreateContextUpscaleFlags
    FFX_UP_HDR = 1 << 0,
    FFX_UP_AUTO_EXPOSURE = 1 << 5,
};
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
enum { FFX_USAGE_READ_ONLY = 0, FFX_USAGE_UAV = 1 << 1, FFX_USAGE_DEPTHTARGET = 1 << 2 };
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
    VkDevice device;
    VkPhysicalDevice phys;
    PFN_vkGetDeviceProcAddr get_device_proc;
} FfxBackendVk;   // both back end descriptions (the current and the deprecated one) have this layout
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
_Static_assert(sizeof(FfxBackendVk) == 40, "ffxCreateBackendVKDesc layout");
// header, command list, 7 resources, jitter and scale, two sizes, sharpen (padded), 3 floats, reset (padded), 4 floats,
// flags
_Static_assert(sizeof(FfxDispatchUpscale) == 16 + 8 + 7 * 48 + 16 + 16 + 4 + 12 + 4 + 16 + 4,
               "ffxDispatchDescUpscale layout");

typedef FfxReturn (*PFN_Create)(FfxContext *, FfxHeader *, const void *);
typedef FfxReturn (*PFN_Destroy)(FfxContext *, const void *);
typedef FfxReturn (*PFN_Query)(FfxContext *, FfxHeader *);
typedef FfxReturn (*PFN_Dispatch)(FfxContext *, const FfxHeader *);

#define CTX_MAX 4

static struct {
    void *lib;
    PFN_Create create;
    PFN_Destroy destroy;
    PFN_Query query;
    PFN_Dispatch dispatch;
    int tried, loaded, failed;
    int old_backend;   // the library takes only the deprecated back end description (a Linux library)
    char path[PLAT_PATH_MAX];
    struct {
        FfxContext ctx;
        uint32_t w, h, ow, oh, flags;
    } ctx[CTX_MAX];
    uint64_t evals;
    VkImage exposure;   // 1x1 R32F holding 1: a Linux library wants an exposure image even for the tonemapped composite
    VmaAllocation exposure_mem;
} F;

int vk_fsr4_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = !strcmp(cfg_upscaling(), "fsr4");
    return wanted;
}

int vk_fsr4_tried(void) { return F.tried; }

int vk_fsr4_available(void) { return F.loaded && !F.failed; }

#ifdef _WIN32
#define FSR4_LIB "amd_fidelityfx_loader_vk.dll"
#else
#define FSR4_LIB "amd_fidelityfx_upscaler_vk.so"
#endif

// The library's path: AC_FSR4_LIB, else FSR4_LIB next to the executable. 0 when the executable's path is unknown.
static int lib_path(char *path, size_t size) {
    const char *own = cfg_get("AC_FSR4_LIB");
    if (own && *own) {
        snprintf(path, size, "%s", own);
        return 1;
    }
    int len = plat_exe_path(path, size - 40);
    char *cut = len ? plat_last_sep(path) : NULL;
    if (!cut) return 0;
    strcpy(cut + 1, FSR4_LIB);
    return 1;
}

static int lib_load(void) {
    if (F.tried) return F.loaded;
    F.tried = 1;
    if (!lib_path(F.path, sizeof F.path)) return 0;
    F.lib = plat_lib_open(F.path);
    if (!F.lib) {
        rt_log("fsr4: cannot load %s (put AMD's FSR 4 library there): FSR 4 off", F.path);
        return 0;
    }
    F.create = (PFN_Create)plat_lib_sym(F.lib, "ffxCreateContext");
    F.destroy = (PFN_Destroy)plat_lib_sym(F.lib, "ffxDestroyContext");
    F.query = (PFN_Query)plat_lib_sym(F.lib, "ffxQuery");
    F.dispatch = (PFN_Dispatch)plat_lib_sym(F.lib, "ffxDispatch");
    if (!F.create || !F.destroy || !F.dispatch) {
        rt_log("fsr4: %s lacks the FSR API's entry points: FSR 4 off", F.path);
        return 0;
    }
    rt_log("fsr4: %s loaded", F.path);
    return F.loaded = 1;
}

// Whether the menu offers FSR 4 on this GPU: an AMD Radeon of the RDNA 2 architecture or newer (AMD's FSR 4.1 runs
// on the Radeon RX 6000 series and newer and on the Ryzen processors with RDNA 2 or newer graphics; told apart by
// VK_KHR_ray_query, which AMD's drivers offer from RDNA 2 on, the first Radeons with ray accelerators), with the
// library there. Whether AMD's library takes this GPU is known only once it runs (an integrated GPU AMD does not list
// fails ffxCreateContext, and FSR 3 runs), so a choice that fails says so in the menu.
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
    char path[PLAT_PATH_MAX];
    int amd = vk_ctx()->props.vendorID == 0x1002, rdna2 = amd && has_ray_query();
    int lib = lib_path(path, sizeof path) && plat_file_exists(path);
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

// The inputs are read as sampled images (COMPUTE_READ, which a Linux library insists on), the output written as storage.
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

// The context for this scene size, output size and colour kind, made at its first use.
static FfxContext *context(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t flags) {
    int free_slot = -1;
    for (int i = 0; i < CTX_MAX; i++) {
        if (F.ctx[i].ctx && F.ctx[i].w == w && F.ctx[i].h == h && F.ctx[i].ow == ow && F.ctx[i].oh == oh &&
            F.ctx[i].flags == flags)
            return &F.ctx[i].ctx;
        if (!F.ctx[i].ctx && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return fail("context (no free slot for another size)", 0), NULL;
    const VkCtx *c = vk_ctx();
    FfxBackendVk be = {.header = {F.old_backend ? FFX_TYPE_BACKEND_VK_OLD : FFX_TYPE_BACKEND_VK, NULL},
                       .device = c->device, .phys = c->phys, .get_device_proc = vkGetDeviceProcAddr};
    FfxCreateUpscale cu = {.header = {FFX_TYPE_CREATE_UPSCALE, &be.header}, .flags = flags,
                           .max_render = {w, h}, .max_upscale = {ow, oh}, .message = on_message};
    uint64_t t0 = plat_now_ns();
    FfxContext ctx = NULL;
    FfxReturn r = F.create(&ctx, &cu.header, NULL);
    if (r != 0 && !F.old_backend) {   // a Linux library knows only the deprecated back end description
        F.old_backend = 1;
        be.header.type = FFX_TYPE_BACKEND_VK_OLD;
        r = F.create(&ctx, &cu.header, NULL);
        if (r == 0) rt_log("fsr4: the library takes the deprecated Vulkan back end description");
    }
    if (r != 0 || !ctx) return fail("context creation", r), NULL;
    F.ctx[free_slot].ctx = ctx;
    F.ctx[free_slot].w = w, F.ctx[free_slot].h = h, F.ctx[free_slot].ow = ow, F.ctx[free_slot].oh = oh;
    F.ctx[free_slot].flags = flags;
    const char *name = "?";
    FfxQueryProviderVersion q = {.header = {FFX_TYPE_QUERY_PROVIDER_VERSION, NULL}};
    if (F.query && F.query(&F.ctx[free_slot].ctx, &q.header) == 0 && q.version_name) name = q.version_name;
    rt_log("fsr4: context %ux%u to %ux%u, %s colour, provider %s, made in %.1f ms", w, h, ow, oh,
           flags & FFX_UP_HDR ? "linear" : "LDR", name, (plat_now_ns() - t0) / 1e6);
    return &F.ctx[free_slot].ctx;
}

int vk_fsr4_evaluate(VkCommandBuffer cb, const Fsr4Eval *e) {
    if (!vk_fsr4_available()) return 0;
    uint32_t ow = e->ow ? e->ow : e->w, oh = e->oh ? e->oh : e->h;
    FfxDispatchUpscale d = {.header = {FFX_TYPE_DISPATCH_UPSCALE, NULL}, .command_list = (void *)cb};
    if (!resource(&d.color, &e->color, FFX_USAGE_READ_ONLY, FFX_STATE_COMPUTE_READ, "scene colour") ||
        !resource(&d.depth, &e->depth, FFX_USAGE_DEPTHTARGET, FFX_STATE_COMPUTE_READ, "scene depth") ||
        !resource(&d.motion, &e->mv, FFX_USAGE_READ_ONLY, FFX_STATE_COMPUTE_READ, "motion vectors") ||
        !resource(&d.output, &e->out, FFX_USAGE_UAV, FFX_STATE_UNORDERED_ACCESS, "output"))
        return fail("resource description", 0);
    if (!exposure_image(cb)) return fail("exposure image", 0);
    Fsr4Image ex = {.image = F.exposure, .format = VK_FORMAT_R32_SFLOAT, .width = 1, .height = 1};
    resource(&d.exposure, &ex, FFX_USAGE_READ_ONLY, FFX_STATE_COMPUTE_READ, "exposure");
    uint32_t flags = e->hdr ? FFX_UP_HDR | FFX_UP_AUTO_EXPOSURE : 0;
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
    inputs_layout(cb, e, 1);
    FfxReturn r = F.dispatch(ctx, &d.header);
    inputs_layout(cb, e, 0);   // also after a refusal: the images stay in GENERAL for FSR 3
    if (r != 0) return fail("dispatch", r);
    if (!F.evals++) rt_log("fsr4: first dispatch recorded (%ux%u to %ux%u)", e->w, e->h, ow, oh);
    return 1;
}

void vk_fsr4_release(void) {
    for (int i = 0; i < CTX_MAX; i++)
        if (F.ctx[i].ctx) F.destroy(&F.ctx[i].ctx, NULL), F.ctx[i].ctx = NULL;
    if (F.exposure) vmaDestroyImage(vk_ctx()->vma, F.exposure, F.exposure_mem), F.exposure = VK_NULL_HANDLE;
}
