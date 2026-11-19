/* The game's window: in place in fullscreen, and borderless fullscreen ([Display] Borderless, "Borderless" in
 * autoexec.cfg).
 *
 * - lithtech.exe creates its window as a 320x200 box in the middle of the desktop (0x402b41..0x402b7b, WS_BORDER).
 *   In fullscreen, d3d.ren only makes it topmost (SetWindowPos(HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE),
 *   RVA 0x23379), so wherever DirectDraw (or a wrapper presenting into the window) does not move it, the picture
 *   starts in the middle of the screen, half of it off the edges, until alt-tab puts things right. The patch hooks
 *   d3d.ren's SetWindowPos and puts the window, without its border, over the whole monitor instead.
 * - Borderless: d3d.ren runs in its windowed mode (its "windowed" variable is read at RVA 0x22faa; the patch makes
 *   it 1 at RVA 0x22fc7), the window is a popup over the whole monitor (its windowed SetWindowPos at RVA 0x23371),
 *   and the frame is shown stretched to the monitor with its aspect ratio kept (black bars for the rest): d3d.ren's
 *   own windowed present (SwapBuffers, RVA 0x38610) copies it 1:1 at the top left of the window. Any resolution
 *   works; the monitor's own is the sharpest. The mouse is mapped from the window to the frame (ui.c asks
 *   lm_window_map_mouse). Alt-tab is an ordinary window switch, with no mode change. */
#include <string.h>
#include "lommpatch.h"

/* d3d.ren RVAs */
#define R_WINDOWED_TEST  0x22fc7u    /* test eax,eax; jz <windowed = 0> */
#define R_SWAP_BUFFERS   0x38610u
#define R_CLEAR_DIRTY    0x3cd60u    /* the dirty rectangle list emptied (cdecl, no arguments) */
#define R_RENDER_TARGET  0x8ce7cu    /* IDirectDrawSurface7 *: what is drawn into */
#define R_PRIMARY_WINDOW 0x8ce80u    /* IDirectDrawSurface7 *: the primary surface in windowed mode */
#define R_DDRAW          0x8ce84u    /* IDirectDraw7 * */
#define R_CLIPPER        0x8e378u    /* IDirectDrawClipper *, created by the first windowed present */
#define R_RENDER_H       0x8da40u
#define R_RENDER_W       0x8da44u
#define R_WINDOWED       0x8da4cu
#define R_HWND           0x8da50u

#define DDBLT_COLORFILL  0x00000400u
#define DDBLT_WAIT       0x01000000u

typedef long (__stdcall *create_clipper_fn)(void *dd, DWORD flags, void **clipper, void *outer);
typedef long (__stdcall *set_hwnd_fn)(void *clipper, DWORD flags, HWND wnd);
typedef long (__stdcall *set_clipper_fn)(void *surface, void *clipper);
typedef long (__stdcall *blt_fn)(void *dest, RECT *dest_rect, void *src, RECT *src_rect, DWORD flags, void *fx);
#define VCALL(obj, off, type) ((type)(*(void ***)(obj))[(off) / 4])

static int g_borderless;
static uintptr_t g_ren;              /* d3d.ren's base, while loaded */
static void (__cdecl *g_swap)(unsigned flags);
static BOOL (WINAPI *real_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);

/* where the frame is in the window's client area, for the mouse: set by each borderless present */
static struct { int on, x, y, w, h, fw, fh; } g_map;

int lm_borderless(void)
{
    return g_borderless;
}

void lm_window_map_mouse(int *x, int *y)
{
    if (!g_map.on || g_map.w <= 0 || g_map.h <= 0) {
        return;
    }
    *x = (int)((long long)(*x - g_map.x) * g_map.fw / g_map.w);
    *y = (int)((long long)(*y - g_map.y) * g_map.fh / g_map.h);
}

static void monitor_rect(HWND wnd, RECT *r)
{
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        *r = mi.rcMonitor;
    } else {
        SetRect(r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
}

/* the window as a popup over its whole monitor */
static BOOL cover_monitor(HWND wnd, HWND after)
{
    RECT m;
    LONG style = GetWindowLongA(wnd, GWL_STYLE);
    LONG want = (style & ~(WS_BORDER | WS_DLGFRAME | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX))
              | WS_POPUP | WS_VISIBLE;
    if (style != want) {
        SetWindowLongA(wnd, GWL_STYLE, want);
    }
    SetWindowLongA(wnd, GWL_EXSTYLE, GetWindowLongA(wnd, GWL_EXSTYLE) & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE
                                                                            | WS_EX_DLGMODALFRAME));
    monitor_rect(wnd, &m);
    return real_SetWindowPos(wnd, after, m.left, m.top, m.right - m.left, m.bottom - m.top,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* d3d.ren's two calls: fullscreen (topmost, no move, no size) and windowed (moved and sized to the frame) */
static BOOL WINAPI hk_SetWindowPos(HWND wnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    if (g_ren && wnd == *(HWND *)(g_ren + R_HWND)) {
        if (*(int *)(g_ren + R_WINDOWED)) {
            if (g_borderless) {
                lm_log("window: borderless over the monitor, the frame %dx%d stretched to it",
                       *(int *)(g_ren + R_RENDER_W), *(int *)(g_ren + R_RENDER_H));
                return cover_monitor(wnd, HWND_TOP);
            }
        } else if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) == (SWP_NOMOVE | SWP_NOSIZE)) {
            lm_log("window: fullscreen, the window moved over the whole monitor");
            return cover_monitor(wnd, after);
        }
    }
    return real_SetWindowPos(wnd, after, x, y, w, h, flags);
}

/* the windowed present, the frame stretched into the window with its aspect ratio kept */
static int borderless_present(void)
{
    void *primary = *(void **)(g_ren + R_PRIMARY_WINDOW), *target = *(void **)(g_ren + R_RENDER_TARGET);
    void *dd = *(void **)(g_ren + R_DDRAW), **clipper = (void **)(g_ren + R_CLIPPER);
    HWND wnd = *(HWND *)(g_ren + R_HWND);
    int fw = *(int *)(g_ren + R_RENDER_W), fh = *(int *)(g_ren + R_RENDER_H), cw, ch, dw, dh;
    RECT client, src, dst, bar;
    POINT o = {0, 0};
    BYTE fx[100];                    /* DDBLTFX */
    if (!primary || !target || fw <= 0 || fh <= 0 || !GetClientRect(wnd, &client)) {
        return 0;
    }
    if (!*clipper && dd && VCALL(dd, 0x10, create_clipper_fn)(dd, 0, clipper, NULL) == 0 && *clipper) {
        VCALL(*clipper, 0x20, set_hwnd_fn)(*clipper, 0, wnd);
        VCALL(primary, 0x70, set_clipper_fn)(primary, *clipper);
    }
    ClientToScreen(wnd, &o);
    cw = client.right - client.left;
    ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) {
        return 1;                    /* minimised: nothing to show */
    }
    if ((long long)cw * fh <= (long long)ch * fw) {
        dw = cw;
        dh = (int)((long long)fh * cw / fw);
    } else {
        dh = ch;
        dw = (int)((long long)fw * ch / fh);
    }
    g_map.x = (cw - dw) / 2;
    g_map.y = (ch - dh) / 2;
    g_map.w = dw;
    g_map.h = dh;
    g_map.fw = fw;
    g_map.fh = fh;
    g_map.on = 1;
    SetRect(&src, 0, 0, fw, fh);
    SetRect(&dst, o.x + g_map.x, o.y + g_map.y, o.x + g_map.x + dw, o.y + g_map.y + dh);
    VCALL(primary, 0x14, blt_fn)(primary, &dst, target, &src, DDBLT_WAIT, NULL);
    memset(fx, 0, sizeof fx);
    *(DWORD *)fx = sizeof fx;        /* dwSize; dwFillColor (black) stays 0 */
    if (g_map.x > 0) {
        SetRect(&bar, o.x, o.y, o.x + g_map.x, o.y + ch);
        VCALL(primary, 0x14, blt_fn)(primary, &bar, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, fx);
        SetRect(&bar, o.x + g_map.x + dw, o.y, o.x + cw, o.y + ch);
        VCALL(primary, 0x14, blt_fn)(primary, &bar, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, fx);
    }
    if (g_map.y > 0) {
        SetRect(&bar, o.x, o.y, o.x + cw, o.y + g_map.y);
        VCALL(primary, 0x14, blt_fn)(primary, &bar, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, fx);
        SetRect(&bar, o.x, o.y + g_map.y + dh, o.x + cw, o.y + ch);
        VCALL(primary, 0x14, blt_fn)(primary, &bar, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, fx);
    }
    ((void (__cdecl *)(void))(g_ren + R_CLEAR_DIRTY))();
    return 1;
}

static void __cdecl hk_swap(unsigned flags)
{
    if (g_borderless && g_ren && *(int *)(g_ren + R_WINDOWED) && borderless_present()) {
        return;
    }
    g_swap(flags);
}

static void on_renderer(HMODULE m)
{
    static const BYTE windowed_test[8] = {0x85, 0xc0, 0x0f, 0x84, 0x83, 0x05, 0x00, 0x00};
    static const BYTE windowed_on[8] = {0xe9, 0x7c, 0x05, 0x00, 0x00, 0x90, 0x90, 0x90};   /* jmp <windowed = 1> */
    static const BYTE swap_head[9] = {0x55, 0x8b, 0xec, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00};
    uintptr_t r = (uintptr_t)m;
    void *old;
    g_ren = r;
    g_map.on = 0;
    old = lm_hook_import_in(m, "USER32.dll", "SetWindowPos", hk_SetWindowPos);
    if (!real_SetWindowPos) {
        real_SetWindowPos = old ? old : (void *)SetWindowPos;
    }
    if (!old) {
        lm_log("window: d3d.ren does not import SetWindowPos as expected, the window is left where it is");
    }
    if (!g_borderless) {
        return;
    }
    if (!lm_patch(r + R_WINDOWED_TEST, windowed_test, windowed_on, sizeof windowed_on)) {
        lm_log("window: d3d.ren's windowed switch differs, no borderless mode");
        return;
    }
    if (!(g_swap = (void (__cdecl *)(unsigned))lm_detour(r + R_SWAP_BUFFERS, swap_head, sizeof swap_head,
                                                         (const void *)hk_swap))) {
        lm_log("window: d3d.ren's SwapBuffers differs, the frame is shown 1:1 at the top left");
    }
}

static void on_renderer_unload(void)
{
    g_ren = 0;
    g_map.on = 0;
}

void lm_window_install(void)
{
    g_borderless = lm_ini_int("Display", "Borderless", 0);
    lm_on_module(LM_MOD_RENDERER, on_renderer);
    lm_on_module_unload(LM_MOD_RENDERER, on_renderer_unload);
    if (g_borderless) {
        lm_log("window: borderless fullscreen");
    }
}
