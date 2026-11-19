/* [Display] HorPlus: the 3-D view at widescreen resolutions.
 *
 * cshell.dll's camera (0x5a785 / 0x5b00a / 0x5b152 RVA) sets fovX = 90 degrees and fovY = fovX * H / W, so the wider
 * the screen, the less you see above and below (Vert-). The engine's SetCameraFOV (0x40917d, cdecl(hCam, fovX,
 * fovY), radians) is detoured to keep the vertical field of view a 4:3 screen has and widen the horizontal one
 * instead (Hor+): with k = (W / H) * 0.75, fovY' = fovY * k and fovX' = 2 atan(tan(fovX / 2) * k). At 16:9 that is 106
 * degrees across instead of 90. GetCameraFOV (0x409149) gives back the values the game set: the underwater wobble
 * reads the field of view, adds to it and sets it again, which would otherwise compound every frame. */
#include <math.h>
#include "lommpatch.h"

#define SET_CAMERA_FOV  0x40917du
#define GET_CAMERA_FOV  0x409149u
#define SCREEN_W        0x490ea0u    /* the screen surface's size (static surface 0x490e90) */
#define SCREEN_H        0x490ea4u

static int (__cdecl *g_set)(void *cam, float fx, float fy);
static int (__cdecl *g_get)(void *cam, float *fx, float *fy);
static struct { void *cam; float fx, fy; } g_cams[4];

static float aspect_factor(void)
{
    int w = *(int *)LM_ADDR(SCREEN_W), h = *(int *)LM_ADDR(SCREEN_H);
    return w > 0 && h > 0 ? (float)w / (float)h * 0.75f : 1.0f;
}

static int __cdecl hk_set(void *cam, float fx, float fy)
{
    float k = aspect_factor();
    int i, free_slot = -1;
    for (i = 0; i < (int)LM_COUNT(g_cams); i++) {
        if (g_cams[i].cam == cam) {
            break;
        }
        if (!g_cams[i].cam && free_slot < 0) {
            free_slot = i;
        }
    }
    if (i == (int)LM_COUNT(g_cams)) {
        i = free_slot >= 0 ? free_slot : 0;
    }
    g_cams[i].cam = cam;
    g_cams[i].fx = fx;
    g_cams[i].fy = fy;
    if (k > 1.0001f) {
        fy *= k;
        fx = 2.0f * atanf(tanf(fx * 0.5f) * k);
    }
    return g_set(cam, fx, fy);
}

static int __cdecl hk_get(void *cam, float *fx, float *fy)
{
    int i, r = g_get(cam, fx, fy);
    for (i = 0; i < (int)LM_COUNT(g_cams); i++) {
        if (cam && g_cams[i].cam == cam) {
            if (fx) *fx = g_cams[i].fx;
            if (fy) *fy = g_cams[i].fy;
            break;
        }
    }
    return r;
}

void lm_widescreen_install(void)
{
    static const BYTE set_head[6] = {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x08};
    static const BYTE get_head[6] = {0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0};
    if (!lm_ini_int("Display", "HorPlus", 1)) {
        return;
    }
    g_get = (int (__cdecl *)(void *, float *, float *))lm_detour(GET_CAMERA_FOV, get_head, sizeof get_head,
                                                                 (const void *)hk_get);
    if (!g_get) {
        lm_log("widescreen: GetCameraFOV (%08x) differs, no Hor+", GET_CAMERA_FOV);
        return;
    }
    g_set = (int (__cdecl *)(void *, float, float))lm_detour(SET_CAMERA_FOV, set_head, sizeof set_head,
                                                             (const void *)hk_set);
    lm_log(g_set ? "widescreen: Hor+ field of view" : "widescreen: SetCameraFOV differs, no Hor+");
}
