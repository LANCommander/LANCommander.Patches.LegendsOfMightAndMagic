/* [Performance] FixMenus ("FastMenus"), the menus' software drawing on the GPU instead (lithtech.exe 1.1).
 *
 * The engine draws text (DrawStringToSurface: GDI into a DIB, then copied into the destination) and FillRect in
 * software: for the screen it locks the WHOLE back buffer (0x40a34d -> RenderStruct LockScreen) for every string and
 * every rectangle, a GPU sync and a read back of the screen each. A menu frame has dozens (Video Options: ~25 strings
 * and ~5 fills, twice once a game has been played: cshell draws the menus a second time outside the 3-D pass), hence
 * 5 fps at high resolutions. And cshell's list and scrollbar backgrounds are screen grabs into surfaces it never
 * optimizes, each drawn with an EndScene / system memory Blt / BeginScene of its own.
 * - Text onto the screen: the two DrawString workers (0x4644d3 char *, 0x463f56 HSTRING; cdecl (dest, font, text,
 *   rect, fore, back, SIZE *measured), after ui.c's font swap and scaling) draw the string once into a surface of its
 *   own, which is optimized (a texture; transparent where the text's background is) and kept, keyed by the font's
 *   HFONT, the colours, the size and the text; drawing it again is a textured quad. Strings not drawn for a while
 *   are let go.
 * - FillRect onto the screen: a small optimized surface of that colour, stretched over the rectangle.
 * - A surface about to be drawn onto the screen (0x40b3ec, called before every blit to it) that cshell never
 *   optimized is optimized, and marked so in cshell's record (the surface's user data: {key, optimized}), which moves
 *   it into the GPU pass. A screen grab into an optimized surface marks it changed (ui.c), so it is uploaded again.
 * The GPU draw needs the renderer in its 3-D and optimized 2-D state (d3d.ren 0x8e1c8, 0x8e1c4): where the menu is
 * not, the draw is bracketed with Start3D / StartOptimized2D / EndOptimized2D / End3D. */
#include <string.h>
#include "lommpatch.h"

#define SCREEN           ((void *)0x490e90u)
#define SCREEN_W         0x490ea0u
#define SCREEN_H         0x490ea4u
#define SCREEN_BPP       0x491004u
#define RENDER_STRUCT    0x491014u
#define CREATE_SURFACE   0x40ae5fu   /* cdecl HSURFACE (w, h) */
#define DELETE_SURFACE   0x40a3e9u   /* cdecl (HSURFACE) */
#define OPTIMIZE_SURFACE 0x40ab27u   /* cdecl LTRESULT (HSURFACE, HLTCOLOR) */
#define FILL_RECT        0x40c1a6u   /* cdecl (dest, rect, color) */
#define START_3D         0x407395u
#define START_OPT_2D     0x407607u
#define END_OPT_2D       0x407687u
#define END_3D           0x407707u
#define WORKER_C         0x4644d3u
#define WORKER_H         0x463f56u
#define BEFORE_BLIT      0x40b3ecu
#define R_IN_3D          0x8e1c8u    /* d3d.ren */
#define R_IN_OPT_2D      0x8e1c4u
#define SURF_W           0x10
#define SURF_H           0x14
#define SURF_FLAGS       0x1c        /* 2 optimized, 4 changed since */
#define SURF_USER        0x24

typedef struct { int left, top, right, bottom; } lt_rect;
typedef int (__cdecl *worker_fn)(void *dest, void *font, const void *text, lt_rect *rect, DWORD fore, DWORD back,
                                 SIZE *size);

static worker_fn g_worker_c, g_worker_h;
static void (__cdecl *g_before_blit)(void *surface);
static int (__cdecl *g_fill)(void *dest, lt_rect *rect, DWORD color);
static CRITICAL_SECTION g_cs;
static int g_on;

/* ---------------------------------------------------------------- the GPU state */

static int g_opened3d, g_opened2d;

static void gpu_begin(void)
{
    BYTE *r = (BYTE *)lm_game_module(LM_MOD_RENDERER);
    g_opened3d = g_opened2d = 0;
    if (!r) {
        return;
    }
    if (!*(int *)(r + R_IN_3D)) {
        ((void (__cdecl *)(void))LM_ADDR(START_3D))();
        g_opened3d = 1;
    }
    if (!*(int *)(r + R_IN_OPT_2D)) {
        ((void (__cdecl *)(void))LM_ADDR(START_OPT_2D))();
        g_opened2d = 1;
    }
}

static void gpu_end(void)
{
    if (g_opened2d) {
        ((void (__cdecl *)(void))LM_ADDR(END_OPT_2D))();
    }
    if (g_opened3d) {
        ((void (__cdecl *)(void))LM_ADDR(END_3D))();
    }
    g_opened3d = g_opened2d = 0;
}

static void *create_surface(int w, int h)
{
    return ((void *(__cdecl *)(int, int))LM_ADDR(CREATE_SURFACE))(w, h);
}

static void delete_surface(void *s)
{
    if (s) {
        ((void (__cdecl *)(void *))LM_ADDR(DELETE_SURFACE))(s);
    }
}

static int optimize(void *s, DWORD key)
{
    return ((int (__cdecl *)(void *, DWORD))LM_ADDR(OPTIMIZE_SURFACE))(s, key) == 0;
}

/* the screen's format and the renderer: a change of either lets go of every cached surface */
static uintptr_t g_epoch[4];

static int renderer_up(void)
{
    return *(void **)LM_ADDR(RENDER_STRUCT) && lm_game_module(LM_MOD_RENDERER);
}

/* ---------------------------------------------------------------- text */

#define TEXT_MAX   200
#define CACHE      384
#define STALE      240               /* frames */

typedef struct {
    void *surface;
    HFONT font;
    DWORD fore, back, key;
    int w, h, len, transparent;
    unsigned hash, used;
    char text[TEXT_MAX];
} entry;

static entry g_cache[CACHE];
static unsigned g_frame;

static void flush(void)
{
    int i;
    for (i = 0; i < CACHE; i++) {
        delete_surface(g_cache[i].surface);
        memset(&g_cache[i], 0, sizeof g_cache[i]);
    }
}

static void check_epoch(void)
{
    uintptr_t now[4];
    now[0] = *(uintptr_t *)LM_ADDR(RENDER_STRUCT);
    now[1] = (uintptr_t)*(int *)LM_ADDR(SCREEN_W);
    now[2] = (uintptr_t)*(int *)LM_ADDR(SCREEN_H);
    now[3] = (uintptr_t)*(int *)LM_ADDR(SCREEN_BPP);
    if (memcmp(now, g_epoch, sizeof now)) {
        flush();
        memcpy(g_epoch, now, sizeof now);
    }
}

/* once a frame: let go of the strings not drawn for a while */
static void on_flip(void)
{
    int i;
    g_frame++;
    if (g_frame % 60) {
        return;
    }
    EnterCriticalSection(&g_cs);
    for (i = 0; i < CACHE; i++) {
        if (g_cache[i].surface && g_frame - g_cache[i].used > STALE) {
            delete_surface(g_cache[i].surface);
            memset(&g_cache[i], 0, sizeof g_cache[i]);
        }
    }
    LeaveCriticalSection(&g_cs);
}

static unsigned fnv(const char *s, int n)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        h = (h ^ (BYTE)s[i]) * 16777619u;
    }
    return h;
}

/* a colour for the transparent parts that is neither the text's nor its background's (exact in 16-bit 565) */
static DWORD pick_key(DWORD fore, DWORD back)
{
    static const DWORD keys[] = {0xff00ff, 0x00ff00, 0x0000ff};
    int i;
    for (i = 0; i < 2; i++) {
        if ((keys[i] != (fore & 0xffffff)) && (keys[i] != (back & 0xffffff))) {
            return keys[i];
        }
    }
    return keys[2];
}

static int draw_cached(worker_fn real, int hstring, void *dest, void *font, const void *text, lt_rect *rect,
                       DWORD fore, DWORD back, SIZE *size)
{
    const char *s;
    lt_rect r, inside;
    entry *e = NULL, *free_slot = NULL, *oldest = NULL;
    HFONT hfont;
    int len, w, h, i, ok;
    unsigned hash;
    if (!g_on || dest != SCREEN || !font || !text || !size || !renderer_up()) {
        if (dest == SCREEN) lm_perf.text_soft++;
        return real(dest, font, text, rect, fore, back, size);
    }
    if (hstring) {
        if (!lm_readable(text, 0x14)) {
            return real(dest, font, text, rect, fore, back, size);
        }
        len = *(const unsigned short *)((const BYTE *)text + 0x10);
        s = (const char *)text + 0x14;
    } else {
        s = (const char *)text;
        len = (int)strlen(s);
    }
    if (rect) {
        r = *rect;
    } else {
        r.left = r.top = 0;
        r.right = *(int *)LM_ADDR(SCREEN_W);
        r.bottom = *(int *)LM_ADDR(SCREEN_H);
    }
    w = size->cx < r.right - r.left ? size->cx : r.right - r.left;
    h = size->cy < r.bottom - r.top ? size->cy : r.bottom - r.top;
    if (len <= 0 || len >= TEXT_MAX || w <= 0 || h <= 0 || w > 2048 || h > 512) {
        lm_perf.text_soft++;
        return real(dest, font, text, rect, fore, back, size);
    }
    hfont = ((HFONT *)font)[2];
    hash = fnv(s, len) ^ (unsigned)(uintptr_t)hfont ^ fore * 31u ^ back * 131u ^ (unsigned)(w << 16 | h);
    EnterCriticalSection(&g_cs);
    check_epoch();
    for (i = 0; i < CACHE; i++) {
        entry *c = &g_cache[i];
        if (!c->surface) {
            if (!free_slot) free_slot = c;
            continue;
        }
        if (c->hash == hash && c->font == hfont && c->fore == fore && c->back == back && c->w == w && c->h == h
            && c->len == len && !memcmp(c->text, s, (size_t)len)) {
            e = c;
            break;
        }
        if (!oldest || c->used < oldest->used) {
            oldest = c;
        }
    }
    if (!e) {
        void *surf;
        DWORD key = pick_key(fore, back);
        int transparent = (back & 0x80000000u) || (fore & 0x80000000u);
        if (!free_slot) {
            delete_surface(oldest->surface);
            memset(oldest, 0, sizeof *oldest);
            free_slot = oldest;
        }
        if (!(surf = create_surface(w, h))) {
            LeaveCriticalSection(&g_cs);
            return real(dest, font, text, rect, fore, back, size);
        }
        if (transparent) {
            ((int (__cdecl *)(void *, lt_rect *, DWORD))LM_ADDR(FILL_RECT))(surf, NULL, key);
        }
        inside.left = inside.top = 0;
        inside.right = w;
        inside.bottom = h;
        real(surf, font, text, &inside, fore, back, size);
        if (!optimize(surf, transparent ? 0x80000000u | key : 0)) {
            delete_surface(surf);
            LeaveCriticalSection(&g_cs);
            return real(dest, font, text, rect, fore, back, size);
        }
        e = free_slot;
        e->surface = surf;
        e->font = hfont;
        e->fore = fore;
        e->back = back;
        e->key = key;
        e->w = w;
        e->h = h;
        e->len = len;
        e->transparent = transparent;
        e->hash = hash;
        memcpy(e->text, s, (size_t)len);
        lm_perf.text_new++;
    } else {
        lm_perf.text_hits++;
    }
    e->used = g_frame;
    gpu_begin();
    ok = (e->transparent ? lm_raw_draw_t(SCREEN, e->surface, NULL, r.left, r.top, e->key)
                         : lm_raw_draw(SCREEN, e->surface, NULL, r.left, r.top)) == 0;
    gpu_end();
    LeaveCriticalSection(&g_cs);
    return ok ? 0 : real(dest, font, text, rect, fore, back, size);
}

static int __cdecl hk_worker_c(void *dest, void *font, const void *text, lt_rect *rect, DWORD fore, DWORD back,
                               SIZE *size)
{
    return draw_cached(g_worker_c, 0, dest, font, text, rect, fore, back, size);
}

static int __cdecl hk_worker_h(void *dest, void *font, const void *text, lt_rect *rect, DWORD fore, DWORD back,
                               SIZE *size)
{
    return draw_cached(g_worker_h, 1, dest, font, text, rect, fore, back, size);
}

/* ---------------------------------------------------------------- FillRect */

#define FILLS 32
static struct { void *surface; DWORD color; } g_fills[FILLS];

/* rect in real screen pixels (NULL: all of it); 1 if drawn */
int lm_fast_fill(void *rect_in, DWORD color)
{
    lt_rect r, *rect = (lt_rect *)rect_in;
    void *surf = NULL;
    int i, ok;
    if (!g_on || (color & 0x80000000u) || !renderer_up()) {
        return 0;
    }
    if (rect) {
        r = *rect;
    } else {
        r.left = r.top = 0;
        r.right = *(int *)LM_ADDR(SCREEN_W);
        r.bottom = *(int *)LM_ADDR(SCREEN_H);
    }
    if (r.right <= r.left || r.bottom <= r.top) {
        return 1;
    }
    EnterCriticalSection(&g_cs);
    check_epoch();
    for (i = 0; i < FILLS && g_fills[i].surface; i++) {
        if (g_fills[i].color == color) {
            surf = g_fills[i].surface;
            break;
        }
    }
    if (!surf) {
        if (i == FILLS) {
            i = (int)(g_frame % FILLS);
            delete_surface(g_fills[i].surface);
            g_fills[i].surface = NULL;
        }
        if (!(surf = create_surface(8, 8))) {
            LeaveCriticalSection(&g_cs);
            return 0;
        }
        ((int (__cdecl *)(void *, lt_rect *, DWORD))LM_ADDR(FILL_RECT))(surf, NULL, color);
        if (!optimize(surf, 0)) {
            delete_surface(surf);
            LeaveCriticalSection(&g_cs);
            return 0;
        }
        g_fills[i].surface = surf;
        g_fills[i].color = color;
    }
    gpu_begin();
    ok = lm_raw_scale(SCREEN, surf, &r, NULL) == 0;
    gpu_end();
    lm_perf.fills += ok;
    LeaveCriticalSection(&g_cs);
    return ok;
}

static int __cdecl hk_fill(void *dest, lt_rect *rect, DWORD color)
{
    if (dest == SCREEN && lm_fast_fill(rect, color)) {
        return 0;
    }
    return g_fill(dest, rect, color);
}

/* ---------------------------------------------------------------- cshell's surfaces */

/* cshell's surfaces it never optimized, the bitmap fonts' glyph sheets above all (one blit per letter, each off the
 * GPU: an EndScene, a system memory Blt and a BeginScene), optimized with the colour key cshell keeps for them */
static void __cdecl hk_before_blit(void *s)
{
    if (g_on && s && s != SCREEN && lm_readable(s, SURF_USER + 4) && !(*(DWORD *)((BYTE *)s + SURF_FLAGS) & 2)) {
        BYTE *ud = *(BYTE **)((BYTE *)s + SURF_USER);
        if (ud && lm_readable(ud, 8) && !ud[4]) {
            int key = *(int *)ud;    /* -1: none; else 0x00RRGGBB, transparent */
            if (optimize(s, key == -1 ? 0 : 0x80000000u | ((DWORD)key & 0xffffff))) {
                ud[4] = 1;           /* cshell's record: optimized (its GPU pass draws it) */
                lm_perf.optimized++;
            }
        }
    }
    g_before_blit(s);
}

void lm_menufast_install(void)
{
    static const BYTE worker_c_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20};
    static const BYTE worker_h_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x24};
    static const BYTE blit_head[5] = {0x56, 0x8b, 0x74, 0x24, 0x08};
    static const BYTE fill_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x78};
    InitializeCriticalSection(&g_cs);
    g_worker_c = (worker_fn)lm_detour(WORKER_C, worker_c_head, sizeof worker_c_head, (const void *)hk_worker_c);
    g_worker_h = (worker_fn)lm_detour(WORKER_H, worker_h_head, sizeof worker_h_head, (const void *)hk_worker_h);
    if (!lm_ui_has_fill()) {         /* else ui.c's FillRect hook asks lm_fast_fill */
        g_fill = (int (__cdecl *)(void *, lt_rect *, DWORD))lm_detour(FILL_RECT, fill_head, sizeof fill_head,
                                                                      (const void *)hk_fill);
    }
    if (lm_ui_marks_grabs()) {       /* optimizing them needs screen grabs to mark them changed (ui.c) */
        g_before_blit = (void (__cdecl *)(void *))lm_detour(BEFORE_BLIT, blit_head, sizeof blit_head,
                                                            (const void *)hk_before_blit);
    }
    lm_on_flip(on_flip);
    g_on = 1;
    lm_log("framerate: menu text%s%s on the GPU", lm_ui_has_fill() || g_fill ? ", fills" : "",
           g_before_blit ? ", cshell's surfaces" : "");
    if (!g_worker_c || !g_worker_h) {
        lm_log("framerate: the engine's text drawing differs, %s left in software",
               !g_worker_c && !g_worker_h ? "all text" : "some text");
    }
}
