/* [Display] ScaleMenus / ScaleHud / HudScale / IntegerScale: the menus and the HUD at any resolution.
 *
 * cshell.dll lays out every menu and HUD element in 640x480 design coordinates and places them with one anchor
 * function (RVA 0x6213a): anchor 0 centres a 640x480 box, 1-7 stick to the screen's edges. The screen size it uses
 * comes from its draw manager's GetScreenRect (RVA 0x755a), and everything is drawn 1:1 in pixels: a small box in the
 * middle of a 4K screen, and a tiny HUD in its corners.
 *
 * The patch gives cshell a virtual screen of W/s x H/s and scales everything it draws on the screen by s:
 * - GetScreenRect answers the virtual size, so the existing anchors centre the menus (aspect ratio kept: bars at the
 *   sides of a wide screen) and put the HUD on the real edges. The check that resets the video mode when it differs
 *   from the screen (RVA 0x3ccbe, returning to the main menu) still gets the real size.
 * - The engine's 2-D functions (lithtech.exe, cdecl, reached from cshell only through the client function table) are
 *   detoured: a draw onto the screen surface (0x490e90) has its destination scaled. DrawSurfaceToSurface[Transparent]
 *   become ScaleSurfaceToSurface[Transparent] (the renderer's hardware blit either way), FillRect's rectangle is
 *   scaled, and DrawStringToSurface draws with a font s times the size (GDI text is pixel sized); the string
 *   measurements stay unscaled, in virtual units, as cshell's layout expects.
 * - Mouse: cshell's window procedure stores the cursor at 0x9fc50 / 0x9fc54 (RVA 0x8902..0x891b); the patch divides
 *   it by s right after (RVA 0x891c), and so the point from ScreenToClient (the cursor's start position, RVA 0x65e42).
 * s: the menus fill the screen's height or width, whichever comes first: min(W/640, H/480) (IntegerScale=1 rounds it
 * down, for sharp pixels). The HUD (drawn while the game screen is the current one, or by it) uses HudScale percent
 * (0 = the same as the menus), kept between 1 and the menus' scale, so the virtual screen is never under 640x480
 * (the anchors subtract unsigned). The engine's own console and the intro videos do not go through this and stay as
 * they were. */
#include <math.h>
#include <string.h>
#include "lommpatch.h"

#define SCREEN_SURFACE  ((void *)0x490e90u)
#define SCREEN_W        0x490ea0u
#define SCREEN_H        0x490ea4u

/* cshell RVAs */
#define CS_GET_SCREEN_RECT 0x755au
#define CS_MODE_CHECK_RET  0x3cce3u   /* the mode check's call returns here: it needs the real size */
#define CS_MOUSE_STORED    0x891cu
#define CS_MOUSE_BACK      0x8921u
#define CS_MOUSE_X         0x9fc50u
#define CS_MOUSE_Y         0x9fc54u
#define CS_SCREEN_TO_CLIENT_IAT 0x82194u
#define CS_GAME_SCREEN     0xa4874u
#define CS_SCREEN_MGR      0xa4cb4u
#define CS_GAME_DRAW_SLOT  0x90054u
#define CS_GAME_DRAW       0x6681fu

typedef struct { int left, top, right, bottom; } lt_rect;

static uintptr_t g_cs;               /* cshell.dll's base, while loaded */
static int g_scale_menus, g_scale_hud, g_hud_percent, g_integer;
static volatile LONG g_in_game_draw;

/* ---------------------------------------------------------------- the scale */

static float max_scale(void)
{
    int w = *(int *)LM_ADDR(SCREEN_W), h = *(int *)LM_ADDR(SCREEN_H);
    float s;
    if (w < 640 || h < 480) {
        return 1.0f;
    }
    s = (float)w / 640.0f < (float)h / 480.0f ? (float)w / 640.0f : (float)h / 480.0f;
    return g_integer ? floorf(s) : s;
}

static int hud_context(void)
{
    void *game, *mgr;
    if (!g_cs) {
        return 0;
    }
    game = *(void **)(g_cs + CS_GAME_SCREEN);
    mgr = *(void **)(g_cs + CS_SCREEN_MGR);
    return game && (g_in_game_draw || (lm_readable(mgr, 8) && *(void **)((BYTE *)mgr + 4) == game));
}

static float scale_now(void)
{
    float smax = max_scale(), s;
    if (!hud_context()) {
        return g_scale_menus ? smax : 1.0f;
    }
    if (!g_scale_hud) {
        return 1.0f;
    }
    s = g_hud_percent > 0 ? g_hud_percent / 100.0f : smax;
    return s < 1.0f ? 1.0f : s > smax ? smax : s;
}

static int R(int v, float s)
{
    return (int)floorf(v * s + 0.5f);
}

static void scale_rect(lt_rect *r, float s)
{
    r->left = R(r->left, s);
    r->top = R(r->top, s);
    r->right = R(r->right, s);
    r->bottom = R(r->bottom, s);
}

/* ---------------------------------------------------------------- the engine's 2-D functions */

typedef int (__cdecl *draw_fn)(void *dest, void *src, lt_rect *src_rect, int x, int y);
typedef int (__cdecl *draw_t_fn)(void *dest, void *src, lt_rect *src_rect, int x, int y, DWORD key);
typedef int (__cdecl *scale_fn)(void *dest, void *src, lt_rect *dst_rect, lt_rect *src_rect);
typedef int (__cdecl *scale_t_fn)(void *dest, void *src, lt_rect *dst_rect, lt_rect *src_rect, DWORD key);
typedef int (__cdecl *fill_fn)(void *dest, lt_rect *rect, DWORD color);
typedef void *(__cdecl *font_fn)(const char *face, int w, int h, int italic, int underline, int bold);
typedef int (__cdecl *delete_font_fn)(void *font);
typedef int (__cdecl *string_fn)(void *dest, void *font, const void *text, lt_rect *rect, DWORD fore, DWORD back);

static draw_fn g_draw;
static draw_t_fn g_draw_t;
static scale_fn g_scale;
static scale_t_fn g_scale_t;
static fill_fn g_fill;
static font_fn g_create_font;
static delete_font_fn g_delete_font;
static string_fn g_string_h, g_string_c;

/* the destination of a Draw: x, y and the source's size */
static void draw_rect(void *src, const lt_rect *src_rect, int x, int y, float s, lt_rect *out)
{
    int w, h;
    if (src_rect) {
        w = src_rect->right - src_rect->left;
        h = src_rect->bottom - src_rect->top;
    } else {
        w = *(int *)((BYTE *)src + 0x10);
        h = *(int *)((BYTE *)src + 0x14);
    }
    out->left = R(x, s);
    out->top = R(y, s);
    out->right = R(x + w, s);
    out->bottom = R(y + h, s);
}

static int to_screen(void *dest, void *src)
{
    return dest == SCREEN_SURFACE && src != SCREEN_SURFACE && src;
}

/* a grab of the screen into a surface: marked changed (flag 4), so an optimized one is uploaded again (menufast.c
 * optimizes cshell's grabs) */
static int grabbed(int r, void *dest, void *src)
{
    if (src == SCREEN_SURFACE && dest && dest != SCREEN_SURFACE && lm_readable(dest, 0x20)) {
        *(DWORD *)((BYTE *)dest + 0x1c) |= 4;
    }
    return r;
}

static int __cdecl hk_draw(void *dest, void *src, lt_rect *src_rect, int x, int y)
{
    float s;
    lt_rect d;
    if (!to_screen(dest, src) || (s = scale_now()) == 1.0f) {
        return grabbed(g_draw(dest, src, src_rect, x, y), dest, src);
    }
    draw_rect(src, src_rect, x, y, s, &d);
    return g_scale(dest, src, &d, src_rect);
}

static int __cdecl hk_draw_t(void *dest, void *src, lt_rect *src_rect, int x, int y, DWORD key)
{
    float s;
    lt_rect d;
    if (!to_screen(dest, src) || (s = scale_now()) == 1.0f) {
        return grabbed(g_draw_t(dest, src, src_rect, x, y, key), dest, src);
    }
    draw_rect(src, src_rect, x, y, s, &d);
    return g_scale_t(dest, src, &d, src_rect, key);
}

/* menufast.c: the engine's functions without the scaling (real screen pixels) */
int lm_raw_draw(void *dest, void *src, void *src_rect, int x, int y)
{
    return (g_draw ? g_draw : (draw_fn)LM_ADDR(0x40b637u))(dest, src, src_rect, x, y);
}

int lm_raw_draw_t(void *dest, void *src, void *src_rect, int x, int y, DWORD key)
{
    return (g_draw_t ? g_draw_t : (draw_t_fn)LM_ADDR(0x40b694u))(dest, src, src_rect, x, y, key);
}

int lm_raw_scale(void *dest, void *src, void *dst_rect, void *src_rect)
{
    return (g_scale ? g_scale : (scale_fn)LM_ADDR(0x40b70au))(dest, src, dst_rect, src_rect);
}

int lm_ui_has_fill(void)
{
    return g_fill != NULL;
}

int lm_ui_marks_grabs(void)
{
    return g_draw && g_draw_t && g_scale && g_scale_t;
}

/* a NULL destination is the whole (virtual) screen */
static void scale_dest(const lt_rect *in, float s, lt_rect *out)
{
    if (in) {
        *out = *in;
    } else {
        out->left = out->top = 0;
        out->right = (int)(*(int *)LM_ADDR(SCREEN_W) / s);
        out->bottom = (int)(*(int *)LM_ADDR(SCREEN_H) / s);
    }
    scale_rect(out, s);
}

static int __cdecl hk_scale(void *dest, void *src, lt_rect *dst_rect, lt_rect *src_rect)
{
    float s;
    lt_rect d;
    if (!to_screen(dest, src) || (s = scale_now()) == 1.0f) {
        return grabbed(g_scale(dest, src, dst_rect, src_rect), dest, src);
    }
    scale_dest(dst_rect, s, &d);
    return g_scale(dest, src, &d, src_rect);
}

static int __cdecl hk_scale_t(void *dest, void *src, lt_rect *dst_rect, lt_rect *src_rect, DWORD key)
{
    float s;
    lt_rect d;
    if (!to_screen(dest, src) || (s = scale_now()) == 1.0f) {
        return grabbed(g_scale_t(dest, src, dst_rect, src_rect, key), dest, src);
    }
    scale_dest(dst_rect, s, &d);
    return g_scale_t(dest, src, &d, src_rect, key);
}

static int __cdecl hk_fill(void *dest, lt_rect *rect, DWORD color)
{
    float s;
    lt_rect d, *r = rect;
    if (dest == SCREEN_SURFACE && rect && (s = scale_now()) != 1.0f) {
        d = *rect;
        scale_rect(&d, s);
        r = &d;
    }
    if (dest == SCREEN_SURFACE && lm_fast_fill(r, color)) {   /* menufast.c: on the GPU */
        return 0;
    }
    return g_fill(dest, r, color);
}

/* ---------------------------------------------------------------- text */

/* a font (the engine's node: [2] is its HFONT), with what it was made from and a copy s times the size */
#define MAX_FONTS 64
static struct {
    void **node;
    char face[64];
    int w, h, italic, underline, bold;
    HFONT scaled;
    float scaled_at;
} g_fonts[MAX_FONTS];
static CRITICAL_SECTION g_font_cs;

static void *__cdecl hk_create_font(const char *face, int w, int h, int italic, int underline, int bold)
{
    void **node = g_create_font(face, w, h, italic, underline, bold);
    int i;
    if (!node) {
        return node;
    }
    EnterCriticalSection(&g_font_cs);
    for (i = 0; i < MAX_FONTS && g_fonts[i].node; i++) {
    }
    if (i < MAX_FONTS) {
        g_fonts[i].node = node;
        lstrcpynA(g_fonts[i].face, face ? face : "", sizeof g_fonts[i].face);
        g_fonts[i].w = w;
        g_fonts[i].h = h;
        g_fonts[i].italic = italic & 0xff;
        g_fonts[i].underline = underline & 0xff;
        g_fonts[i].bold = bold & 0xff;
        g_fonts[i].scaled = NULL;
    }
    LeaveCriticalSection(&g_font_cs);
    return node;
}

static int __cdecl hk_delete_font(void *font)
{
    int i;
    EnterCriticalSection(&g_font_cs);
    for (i = 0; i < MAX_FONTS; i++) {
        if (font && g_fonts[i].node == font) {
            if (g_fonts[i].scaled) {
                DeleteObject(g_fonts[i].scaled);
            }
            memset(&g_fonts[i], 0, sizeof g_fonts[i]);
        }
    }
    LeaveCriticalSection(&g_font_cs);
    return g_delete_font(font);
}

/* the font's HFONT at scale s (in the critical section) */
static HFONT scaled_font(void *font, float s)
{
    int i;
    for (i = 0; i < MAX_FONTS; i++) {
        if (font && g_fonts[i].node == font) {
            if (!g_fonts[i].scaled || g_fonts[i].scaled_at != s) {
                if (g_fonts[i].scaled) {
                    DeleteObject(g_fonts[i].scaled);
                }
                /* as the engine makes them (0x463d49) */
                g_fonts[i].scaled = CreateFontA(R(g_fonts[i].h, s), R(g_fonts[i].w, s), 0, 0,
                                                g_fonts[i].bold ? 700 : 400, g_fonts[i].italic, g_fonts[i].underline,
                                                0, 1, 0, 0, 0, 0, g_fonts[i].face);
                g_fonts[i].scaled_at = s;
            }
            return g_fonts[i].scaled;
        }
    }
    return NULL;
}

static int draw_string(string_fn real, void *dest, void *font, const void *text, lt_rect *rect, DWORD fore, DWORD back)
{
    float s;
    lt_rect d;
    HFONT big, old;
    int r;
    if (dest != SCREEN_SURFACE || !rect || !font || (s = scale_now()) == 1.0f) {
        return real(dest, font, text, rect, fore, back);
    }
    d = *rect;
    scale_rect(&d, s);
    EnterCriticalSection(&g_font_cs);
    if (!(big = scaled_font(font, s))) {
        LeaveCriticalSection(&g_font_cs);
        return real(dest, font, text, &d, fore, back);
    }
    old = ((HFONT *)font)[2];
    ((HFONT *)font)[2] = big;
    r = real(dest, font, text, &d, fore, back);
    ((HFONT *)font)[2] = old;
    LeaveCriticalSection(&g_font_cs);
    return r;
}

static int __cdecl hk_string_h(void *dest, void *font, const void *text, lt_rect *rect, DWORD fore, DWORD back)
{
    return draw_string(g_string_h, dest, font, text, rect, fore, back);
}

static int __cdecl hk_string_c(void *dest, void *font, const void *text, lt_rect *rect, DWORD fore, DWORD back)
{
    return draw_string(g_string_c, dest, font, text, rect, fore, back);
}

/* ---------------------------------------------------------------- cshell */

/* DrawMgr::GetScreenRect: thiscall(this, LTRect *out), returns out */
static lt_rect *__attribute__((thiscall)) hk_screen_rect(void *self, lt_rect *out)
{
    int w = *(int *)LM_ADDR(SCREEN_W), h = *(int *)LM_ADDR(SCREEN_H);
    float s = scale_now();
    (void)self;
    out->left = out->top = 0;
    if ((uintptr_t)__builtin_return_address(0) == g_cs + CS_MODE_CHECK_RET || s == 1.0f) {
        out->right = w;
        out->bottom = h;
    } else {
        out->right = (int)(w / s);
        out->bottom = (int)(h / s);
        if (out->right < 640) out->right = 640;
        if (out->bottom < 480) out->bottom = 480;
    }
    return out;
}

/* the window procedure stored the cursor (ecx = the message) */
void __cdecl lm_ui_mouse(unsigned msg)
{
    float s;
    if (msg >= 0x200 && msg <= 0x209 && g_cs) {
        int *x = (int *)(g_cs + CS_MOUSE_X), *y = (int *)(g_cs + CS_MOUSE_Y);
        lm_window_map_mouse(x, y);   /* borderless: from the window to the frame */
        if ((s = scale_now()) != 1.0f) {
            *x = (int)floorf(*x / s);
            *y = (int)floorf(*y / s);
        }
    }
}

void *lm_ui_mouse_back;
extern void lm_ui_mouse_entry(void);
__asm__(".text\n"
        ".globl _lm_ui_mouse_entry\n"
        "_lm_ui_mouse_entry:\n"
        "    pushal\n"
        "    pushl %ecx\n"
        "    call _lm_ui_mouse\n"
        "    addl $4, %esp\n"
        "    popal\n"
        "    movl $0x203, %eax\n"         /* the instruction replaced */
        "    jmp *_lm_ui_mouse_back\n");

static BOOL (WINAPI *real_ScreenToClient)(HWND, LPPOINT);

static BOOL WINAPI hk_ScreenToClient(HWND wnd, LPPOINT pt)
{
    BOOL r = real_ScreenToClient(wnd, pt);
    float s = scale_now();
    if (r && pt) {
        int x = pt->x, y = pt->y;
        lm_window_map_mouse(&x, &y);
        pt->x = x;
        pt->y = y;
    }
    if (r && pt && s != 1.0f) {
        pt->x = (LONG)floorf(pt->x / s);
        pt->y = (LONG)floorf(pt->y / s);
    }
    return r;
}

/* the game screen's draw (3-D view and HUD): its HUD context, whatever screen is current */
static void (__attribute__((thiscall)) *g_game_draw)(void *self);

static void __attribute__((thiscall)) hk_game_draw(void *self)
{
    InterlockedIncrement(&g_in_game_draw);
    g_game_draw(self);
    InterlockedDecrement(&g_in_game_draw);
}

static void on_cshell(HMODULE m)
{
    static const BYTE rect_head[5] = {0x55, 0x8b, 0xec, 0x51, 0x51};
    static const BYTE mouse_at[5] = {0xb8, 0x03, 0x02, 0x00, 0x00};
    static const BYTE stores[26] = {0x0f, 0xbf, 0x45, 0x14, 0xa3, 0, 0, 0, 0, 0x8b, 0x45, 0x14, 0x8b, 0x4d, 0x0c,
                                    0xc1, 0xe8, 0x10, 0x0f, 0xbf, 0xc0, 0xa3, 0, 0, 0, 0};
    uintptr_t cs = (uintptr_t)m;
    BYTE expect[26];
    uint32_t x = (uint32_t)(cs + CS_MOUSE_X), y = (uint32_t)(cs + CS_MOUSE_Y);
    g_cs = cs;
    if ((g_scale_menus || g_scale_hud)
        && (!lm_expect(cs + CS_GET_SCREEN_RECT, rect_head, sizeof rect_head)
            || !lm_jmp(cs + CS_GET_SCREEN_RECT, (const void *)hk_screen_rect))) {
        lm_log("ui: cshell's GetScreenRect differs, menus and HUD stay unscaled");
        g_scale_menus = g_scale_hud = 0;   /* scale_now() is 1 from here on; the mouse hooks still serve borderless */
    }
    /* the mouse: the stores of the window procedure (relocated addresses), then mov eax,0x203 */
    memcpy(expect, stores, sizeof expect);
    memcpy(expect + 5, &x, 4);
    memcpy(expect + 22, &y, 4);
    lm_ui_mouse_back = (void *)(cs + CS_MOUSE_BACK);
    if (!lm_expect(cs + 0x8902u, expect, sizeof expect) || !lm_expect(cs + CS_MOUSE_STORED, mouse_at, sizeof mouse_at)
        || !lm_jmp(cs + CS_MOUSE_STORED, (const void *)lm_ui_mouse_entry)) {
        lm_log("ui: cshell's window procedure differs, the mouse is not scaled");
    }
    if (lm_readable((void *)(cs + CS_SCREEN_TO_CLIENT_IAT), 4)
        && *(void **)(cs + CS_SCREEN_TO_CLIENT_IAT) == (void *)ScreenToClient) {
        real_ScreenToClient = ScreenToClient;
        lm_write(cs + CS_SCREEN_TO_CLIENT_IAT, &(void *){(void *)hk_ScreenToClient}, 4);
    } else {
        real_ScreenToClient = lm_hook_import_in(m, "USER32.dll", "ScreenToClient", hk_ScreenToClient);
        if (!real_ScreenToClient) real_ScreenToClient = ScreenToClient;
    }
    g_game_draw = (void (__attribute__((thiscall)) *)(void *))(cs + CS_GAME_DRAW);
    if (!lm_hook_slot(cs + CS_GAME_DRAW_SLOT, cs + CS_GAME_DRAW, (const void *)hk_game_draw)) {
        lm_log("ui: the game screen's draw differs, the HUD uses its scale only while no menu is open");
    }
    lm_log("ui: menus scaled %s, HUD %s (screen %dx%d)", g_scale_menus ? "to fit" : "off",
           !g_scale_hud ? "unscaled" : g_hud_percent ? "at HudScale" : "as the menus",
           *(int *)LM_ADDR(SCREEN_W), *(int *)LM_ADDR(SCREEN_H));
}

static void *detour(uintptr_t fn, const BYTE *head, size_t len, const void *hook, const char *what)
{
    void *t = lm_detour(fn, head, len, hook);
    if (!t) {
        lm_log("ui: %s (%08lx) differs, left alone", what, (unsigned long)fn);
    }
    return t;
}

void lm_ui_install(void)
{
    static const BYTE draw_head[10] = {0x55, 0x8b, 0xec, 0x83, 0x3d, 0x14, 0x10, 0x49, 0x00, 0x00};
    static const BYTE scale_head[8] = {0x33, 0xc0, 0x39, 0x05, 0x14, 0x10, 0x49, 0x00};
    static const BYTE fill_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x78};
    static const BYTE font_head[5] = {0x55, 0x8b, 0xec, 0x56, 0x57};
    static const BYTE delete_head[5] = {0x83, 0x7c, 0x24, 0x04, 0x00};
    static const BYTE string_head[5] = {0x55, 0x8b, 0xec, 0x51, 0x51};
    g_scale_menus = lm_ini_int("Display", "ScaleMenus", 1);
    g_scale_hud = lm_ini_int("Display", "ScaleHud", 1);
    g_hud_percent = lm_ini_int("Display", "HudScale", 0);
    g_integer = lm_ini_int("Display", "IntegerScale", 0);
    if (!g_scale_menus && !g_scale_hud) {
        if (lm_borderless()) {
            lm_on_module(LM_MOD_CSHELL, on_cshell);   /* the mouse, from the window to the frame */
        }
        return;
    }
    InitializeCriticalSection(&g_font_cs);
    /* the Scale functions first: the Draw hooks call their originals */
    if (!(g_scale = detour(0x40b70a, scale_head, sizeof scale_head, (const void *)hk_scale, "ScaleSurfaceToSurface"))
        || !(g_scale_t = detour(0x40b984, draw_head, sizeof draw_head, (const void *)hk_scale_t,
                                "ScaleSurfaceToSurfaceTransparent"))
        || !(g_draw = detour(0x40b637, draw_head, sizeof draw_head, (const void *)hk_draw, "DrawSurfaceToSurface"))
        || !(g_draw_t = detour(0x40b694, draw_head, sizeof draw_head, (const void *)hk_draw_t,
                               "DrawSurfaceToSurfaceTransparent"))) {
        return;
    }
    g_fill = detour(0x40c1a6, fill_head, sizeof fill_head, (const void *)hk_fill, "FillRect");
    if ((g_create_font = detour(0x463d49, font_head, sizeof font_head, (const void *)hk_create_font, "CreateFont"))
        && (g_delete_font = detour(0x463da8, delete_head, sizeof delete_head, (const void *)hk_delete_font,
                                   "DeleteFont"))) {
        g_string_h = detour(0x463ed5, string_head, sizeof string_head, (const void *)hk_string_h, "DrawStringToSurface");
        g_string_c = detour(0x46445c, string_head, sizeof string_head, (const void *)hk_string_c, "DrawStringToSurface");
    }
    lm_on_module(LM_MOD_CSHELL, on_cshell);
}

/* ---------------------------------------------------------------- the patch's own text on the screen */

/* through the engine's own entry points, so ui.c's scaling (when it is on) applies as to cshell's text */
void *lm_engine_font(const char *face, int height, int bold)
{
    return ((font_fn)LM_ADDR(0x463d49u))(face, 0, height, 0, 0, bold);
}

void lm_engine_text(int x, int y, int w, int h, void *font, const char *text, DWORD rgb)
{
    lt_rect r;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    ((string_fn)LM_ADDR(0x46445cu))(SCREEN_SURFACE, font, text, &r, rgb & 0xffffff, 0x80000000u);
}
