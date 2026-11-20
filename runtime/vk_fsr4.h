// AMD FSR 4 (vk_fsr4.c): AMD FSR Upscaling 4 (the ML upscaler) as the scene's temporal upscaler, at its own size (Native
// AA) or from a smaller scene, behind AC_UPSCALING=fsr4 (astralchain.ini upscaling = fsr4). vk_dlaa.c finds the scene,
// jitters it, makes its motion vectors and calls vk_fsr4_evaluate where the upscaler runs; this file only talks to AMD's
// FSR API (ffxCreateContext / ffxDispatch), loaded at run time from beside the executable: amd_fidelityfx_loader_vk.dll
// with amd_fidelityfx_upscaler_vk.dll next to it (AMD's signed DLLs of the FSR SDK 2.4.0, FSR 4.1.2), on Windows
// directly, on Linux through ffx-vk-linux (libffx_vk_linux.so, which runs the same DLLs). Where FSR 4 cannot run (a
// file missing, a GPU AMD's library does not take), vk_dlaa.c has FSR 3 run in its place.
#pragma once
#include "vk.h"

int vk_fsr4_wanted(void);      // the setting is fsr4 at start
int vk_fsr4_tried(void);       // the library was looked for in this run (it was chosen at start)
void vk_fsr4_init(void);       // vk_dlaa_init, the device up: loads the library when the setting is fsr4
int vk_fsr4_available(void);   // loaded and not failed in this run (a library that made another upscaler than FSR 4
                               // for this GPU has failed)
int vk_fsr4_offered(void);     // the menu lists FSR 4: an AMD RDNA 2 or newer GPU, the library's files there (after init)
const char *vk_fsr4_missing(void);   // the name of the file FSR 4 needs that is not beside the executable, NULL when
                                     // none is missing (after init)

// boot.c through vk_dlaa_warm, the sizes settled and no frame made yet: makes the context of this scene size (w x h),
// output size (ow x oh) and colour kind, so its cost (the library compiles its shaders) is not paid at the first frame.
void vk_fsr4_warm(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int hdr);

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t width, height;   // the image's own extent (level 0)
    uint32_t level, layer;    // the subresource the scene is in; FSR's API takes level 0, layer 0 only
} Fsr4Image;

typedef struct {
    Fsr4Image color, depth, mv, out;   // as Fsr3Eval: the scene, its depth, RG16F motion (render pixels, last frame's
                                       // position minus this one's, without jitter), the RGBA16F output (ow x oh)
    uint32_t w, h;                     // the scene's size (the input)
    uint32_t ow, oh;                   // the output's size, 0: the scene's (Native AA)
    float jx, jy;                      // this frame's jitter, pixels (window x right, y down)
    float dt_ms;
    float near_z, far_z, fov_y;
    int reset;
    int hdr;                           // color is linear scene light (auto exposure), else 0..1 (tonemapped)
} Fsr4Eval;

// Records FSR 4 into cb (the replay's command buffer, no render pass open; vk_dlaa.c drains the recorder first, as for
// XeSS; every image is in GENERAL before and after). 0: FSR 4 failed and is off for the rest of the run.
int vk_fsr4_evaluate(VkCommandBuffer cb, const Fsr4Eval *e);
// vk_dlaa_frame, at each present on the replay thread: sweep destroys the contexts that went once no frame in flight
// uses them; trim, when FSR 4 has not run for a while, has every context go (the next evaluation makes its own again).
void vk_fsr4_sweep(void);
void vk_fsr4_trim(void);
void vk_fsr4_release(void);   // vk_dlaa_release, the device idle: every context goes
