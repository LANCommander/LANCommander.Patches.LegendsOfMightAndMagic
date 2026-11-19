/* [Performance]: the frame rate drops in the menus and while the console is open (lithtech.exe 1.1; the renderer
 * d3d.ren's function table, the RenderStruct, is reached through the engine's pointer at [0x491014]).
 *
 * - TimerResolution: nothing in the game calls timeBeginPeriod, so the MaxFPS limiter (0x40d6cb, Sleep(1) in a
 *   timeGetTime loop) sleeps a whole 15.6 ms timer tick at a time: the light menu frames come out at ~50 fps with
 *   15/31 ms judder instead of MaxFPS. timeBeginPeriod(1) for the life of the game, and PreciseLimiter replaces the
 *   limiter with one that keeps an absolute deadline on the performance counter (a high resolution waitable timer,
 *   then a short spin), so MaxFPS is what you get.
 * - FixMenus: the menus present with FlipScreen's "dirty rectangles" flag (4), and the renderer (SwapBuffers, d3d.ren
 *   RVA 0x38610 -> 0x3cbc0) then copies every changed rectangle to the primary surface with its own Blt (DDBLT_WAIT):
 *   up to 98 per frame, one per character of bitmap-font text. On today's composited desktop (and under DirectDraw
 *   wrappers) each Blt to the primary is a present of its own. FlipScreen (0x407258) turns flag 4 into flag 2: one
 *   copy of the whole back buffer, which keeps the back buffer's contents as the menus' incremental redraw expects.
 * - FixConsole: ConsoleDraw (0x417558) draws each visible line of text with a Lock / software font blit / Unlock of
 *   the back buffer (0x417699): ~100 back-buffer locks per frame at 4K, each one a GPU sync and a read back. The two
 *   calls that draw the lines and the input line (0x41763b, 0x417643) are bracketed so that the RenderStruct's
 *   LockScreen / UnlockScreen (+0xd4 / +0xd8) lock the console's rectangle once and hand out pointers into it. Also,
 *   with ConsoleAlpha 1 (the default) the console background is stretched from system memory to the back buffer in
 *   software every frame; at 0.999 the engine draws it as textured quads instead (the difference is not visible). */
#include <math.h>
#include <string.h>
#include "lommpatch.h"

#define RENDER_STRUCT   0x491014u    /* RenderStruct * */
#define RS_LOCK_SCREEN  0xd4
#define RS_UNLOCK_SCREEN 0xd8
#define SCREEN_BPP      0x491004u    /* bytes per pixel of the screen */
#define CONSOLE         0x493098u
#define CONSOLE_RS      0x268        /* the console's copy of the RenderStruct pointer */
#define CONSOLE_ALPHA   0x3a8        /* float, refreshed from the cvar every frame */
#define CONSOLE_ALPHA_DEFAULT 0x486198u
#define CONSOLE_RECT_FN 0x417905u    /* thiscall(console, int rect[4]) */
#define MAX_FPS         0x493804u    /* float */
#define FLIP_SCREEN     0x407258u
#define LIMITER         0x40d6cbu
#define CONSOLE_LINES_CALL 0x41763bu
#define CONSOLE_LINES   0x4172e8u
#define CONSOLE_INPUT_CALL 0x417643u
#define CONSOLE_INPUT   0x41635eu

typedef int (__cdecl *lock_fn)(int x1, int y1, int x2, int y2, void **bits, int *pitch);
typedef int (__cdecl *unlock_fn)(void);
static BYTE *g_rs;                   /* the RenderStruct whose slots the console's batch has swapped, while it does */
static unsigned __cdecl hk_flip_inner(unsigned flags);

/* ---------------------------------------------------------------- timer and limiter */

static LARGE_INTEGER g_freq;
static LONGLONG g_next;
static HANDLE g_timer;

/* the engine's limiter: cdecl(work time of the frame in microseconds); its clamp of MaxFPS to 10..200 is kept */
static void __cdecl limiter(unsigned work_us)
{
    float fps = *(volatile float *)LM_ADDR(MAX_FPS);
    LARGE_INTEGER now, start;
    LONGLONG period, left;
    (void)work_us;
    QueryPerformanceCounter(&start);
    if (!(fps > 0.0f)) {
        g_next = 0;
        return;
    }
    if (fps < 10.0f) {
        fps = 10.0f;
    } else if (fps > 200.0f) {
        fps = 200.0f;
    }
    period = (LONGLONG)(g_freq.QuadPart / fps);
    QueryPerformanceCounter(&now);
    if (!g_next || now.QuadPart - g_next > period || g_next + period - now.QuadPart > 2 * period) {
        g_next = now.QuadPart;       /* the first frame, or this one is late already: start over, no wait */
        return;
    }
    g_next += period;
    while ((left = g_next - now.QuadPart) > 0) {
        LONGLONG ms = left * 1000 / g_freq.QuadPart;
        if (ms >= 2) {
            if (g_timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(left - g_freq.QuadPart / 1000) * 10000000 / g_freq.QuadPart;   /* 100 ns, relative */
                SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(g_timer, 100);
            } else {
                Sleep((DWORD)ms - 1);
            }
        } else {
            YieldProcessor();
        }
        QueryPerformanceCounter(&now);
    }
    lm_perf.limiter_ticks += now.QuadPart - start.QuadPart;
}

static void install_limiter(void)
{
    static const BYTE head[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c};   /* push ebp; mov ebp,esp; sub esp,0Ch */
    HANDLE (WINAPI *create)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    QueryPerformanceFrequency(&g_freq);
    create = (HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD))(void (*)(void))GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExW");
    if (create) {
        g_timer = create(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!g_timer) {
            g_timer = create(NULL, NULL, 0, TIMER_ALL_ACCESS);   /* before Windows 10 1803 */
        }
    }
    if (lm_expect(LIMITER, head, sizeof head) && lm_jmp(LIMITER, (const void *)limiter)) {
        lm_log("framerate: precise MaxFPS limiter");
    } else {
        lm_log("framerate: the frame limiter (%08x) differs, left alone", LIMITER);
    }
}

/* ---------------------------------------------------------------- FlipScreen */

static int g_fix_menus, g_fix_console;
static unsigned (__cdecl *g_flip)(unsigned flags);

static void (*g_on_flip[4])(void);
static int g_n_on_flip;

void lm_on_flip(void (*fn)(void))
{
    if (g_n_on_flip < (int)LM_COUNT(g_on_flip)) {
        g_on_flip[g_n_on_flip++] = fn;
    }
}

/* ---------------------------------------------------------------- PerfLog: where a frame's time goes */

typedef int (__cdecl *blit_fn)(void *a);

static int g_perf;
static LARGE_INTEGER g_pf;
static LONGLONG g_last_flip, g_window_start;
lm_perf_counters lm_perf;
static BYTE *g_perf_rs;              /* the RenderStruct whose slots are wrapped */
static lock_fn g_perf_lock;
static unlock_fn g_perf_unlock;
static blit_fn g_perf_blit;

static LONGLONG perf_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double perf_ms(LONGLONG ticks)
{
    return g_pf.QuadPart ? ticks * 1000.0 / g_pf.QuadPart : 0.0;
}

static int __cdecl perf_lock(int x1, int y1, int x2, int y2, void **bits, int *pitch)
{
    LONGLONG t = perf_now();
    int r = g_perf_lock(x1, y1, x2, y2, bits, pitch);
    lm_perf.locks++;
    lm_perf.lock_ticks += perf_now() - t;
    return r;
}

static int __cdecl perf_unlock(void)
{
    LONGLONG t = perf_now();
    int r = g_perf_unlock();
    lm_perf.lock_ticks += perf_now() - t;
    return r;
}

/* BlitToScreen (cdecl, one argument: the blit's description) */
static int __cdecl perf_blit(void *a)
{
    LONGLONG t = perf_now();
    BYTE *ren = (BYTE *)lm_game_module(LM_MOD_RENDERER), *rs = a ? *(BYTE **)a : NULL;
    int r;
    /* the GPU path needs an optimized surface (+0x84) inside Start3D and StartOptimized2D (d3d.ren 0x8e1c8/0x8e1c4) */
    if (ren && (!rs || !*(void **)(rs + 0x84) || !*(int *)(ren + 0x8e1c8) || !*(int *)(ren + 0x8e1c4))) {
        lm_perf.slow_blits++;
    }
    r = g_perf_blit(a);
    lm_perf.blits++;
    lm_perf.blit_ticks += perf_now() - t;
    return r;
}

/* the renderer's LockScreen / UnlockScreen / BlitToScreen wrapped (again after the renderer is set up again) */
static void perf_wrap(void)
{
    BYTE *rs = *(BYTE **)LM_ADDR(RENDER_STRUCT);
    if (!lm_readable(rs, 0xe0) || g_rs) {
        return;                      /* no renderer, or the console's batch has the slots */
    }
    if (rs != g_perf_rs || *(lock_fn *)(rs + RS_LOCK_SCREEN) != perf_lock) {
        g_perf_lock = *(lock_fn *)(rs + RS_LOCK_SCREEN);
        g_perf_unlock = *(unlock_fn *)(rs + RS_UNLOCK_SCREEN);
        g_perf_blit = *(blit_fn *)(rs + 0xdc);
        if (!g_perf_lock || !g_perf_unlock || !g_perf_blit) {
            return;
        }
        *(lock_fn *)(rs + RS_LOCK_SCREEN) = perf_lock;
        *(unlock_fn *)(rs + RS_UNLOCK_SCREEN) = perf_unlock;
        *(blit_fn *)(rs + 0xdc) = perf_blit;
        g_perf_rs = rs;
    }
}

static void perf_frame(LONGLONG flip_start, LONGLONG flip_end, unsigned flags)
{
    LONGLONG span;
    if (g_last_flip) {
        lm_perf.frames++;
        lm_perf.frame_ticks += flip_start - g_last_flip;
        if (flip_start - g_last_flip > lm_perf.worst_ticks) {
            lm_perf.worst_ticks = flip_start - g_last_flip;
        }
    }
    lm_perf.present_ticks += flip_end - flip_start;
    lm_perf.flags_or |= flags;
    g_last_flip = flip_end;
    if (!g_window_start) {
        g_window_start = flip_end;
    }
    span = flip_end - g_window_start;
    if (perf_ms(span) >= 5000.0 && lm_perf.frames) {
        double n = (double)lm_perf.frames;
        lm_log("perf: %.1f fps (worst %.1f ms) per frame: %.2f ms = present %.2f + limiter %.2f + the rest %.2f;"
               " screen locks %.1f (%.2f ms), blits %.1f (%.1f not on the GPU; %.2f ms); text %.1f cached, %.1f new, %.1f in software;"
               " fills %.1f; surfaces optimized %u; %dx%d, flip flags %x, %s",
               n / (perf_ms(span) / 1000.0), perf_ms(lm_perf.worst_ticks), perf_ms(lm_perf.frame_ticks) / n,
               perf_ms(lm_perf.present_ticks) / n, perf_ms(lm_perf.limiter_ticks) / n,
               (perf_ms(lm_perf.frame_ticks) - perf_ms(lm_perf.present_ticks) - perf_ms(lm_perf.limiter_ticks)) / n,
               lm_perf.locks / n, perf_ms(lm_perf.lock_ticks) / n, lm_perf.blits / n, lm_perf.slow_blits / n,
               perf_ms(lm_perf.blit_ticks) / n,
               lm_perf.text_hits / n, lm_perf.text_new / n, lm_perf.text_soft / n, lm_perf.fills / n, lm_perf.optimized,
               *(int *)LM_ADDR(0x490ea0u), *(int *)LM_ADDR(0x490ea4u), lm_perf.flags_or,
               lm_borderless() ? "borderless" : "fullscreen");
        memset(&lm_perf, 0, sizeof lm_perf);
        g_window_start = flip_end;
    }
}

static unsigned __cdecl hk_flip(unsigned flags)
{
    int i;
    LONGLONG t0 = 0;
    unsigned r;
    for (i = 0; i < g_n_on_flip; i++) {
        g_on_flip[i]();
    }
    if (g_perf) {
        perf_wrap();
        t0 = perf_now();
        r = hk_flip_inner(flags);
        perf_frame(t0, perf_now(), flags);
        return r;
    }
    return hk_flip_inner(flags);
}

static unsigned __cdecl hk_flip_inner(unsigned flags)
{
    if (g_fix_menus && (flags & 4)) {
        flags = (flags & ~4u) | 2u;  /* one copy of the whole back buffer, not one per dirty rectangle */
    }
    if (g_fix_console) {
        float *alpha = (float *)LM_ADDR(CONSOLE + CONSOLE_ALPHA);
        if (*alpha == 1.0f) {
            *alpha = 0.999f;         /* the background as textured quads, not a software stretch */
        }
    }
    return g_flip(flags);
}

/* ---------------------------------------------------------------- console text: one lock */

static lock_fn g_real_lock;
static unlock_fn g_real_unlock;
static struct { int locked, x1, y1, x2, y2, pitch; BYTE *bits; } g_batch;

static int __cdecl batch_lock(int x1, int y1, int x2, int y2, void **bits, int *pitch)
{
    int bpp = *(int *)LM_ADDR(SCREEN_BPP), r;
    if (bpp < 1 || bpp > 4) {
        bpp = 4;
    }
    if (g_batch.locked && (x1 < g_batch.x1 || y1 < g_batch.y1 || x2 > g_batch.x2 || y2 > g_batch.y2)) {
        g_real_unlock();             /* outside the console: back to one lock per call */
        g_batch.locked = 0;
    }
    if (!g_batch.locked) {
        int rect[4];
        ((void (__attribute__((thiscall)) *)(void *, int *))LM_ADDR(CONSOLE_RECT_FN))(LM_ADDR(CONSOLE), rect);
        g_batch.x1 = rect[0] < x1 ? rect[0] : x1;
        g_batch.y1 = rect[1] < y1 ? rect[1] : y1;
        g_batch.x2 = rect[2] > x2 ? rect[2] : x2;
        g_batch.y2 = rect[3] > y2 ? rect[3] : y2;
        if (g_batch.x1 < 0) g_batch.x1 = 0;
        if (g_batch.y1 < 0) g_batch.y1 = 0;
        r = g_real_lock(g_batch.x1, g_batch.y1, g_batch.x2, g_batch.y2, (void **)&g_batch.bits, &g_batch.pitch);
        if (!g_batch.bits) {
            return r;                /* failed: the caller sees the failure */
        }
        g_batch.locked = 1;
    }
    *bits = g_batch.bits + (y1 - g_batch.y1) * g_batch.pitch + (x1 - g_batch.x1) * bpp;
    *pitch = g_batch.pitch;
    return 0;                        /* LT_OK */
}

static int __cdecl batch_unlock(void)
{
    return 0;
}

void __cdecl lm_console_begin(void)
{
    BYTE *rs = *(BYTE **)LM_ADDR(CONSOLE + CONSOLE_RS);
    if (!g_fix_console || !lm_readable(rs, RS_UNLOCK_SCREEN + 4) || g_rs) {
        return;
    }
    g_real_lock = *(lock_fn *)(rs + RS_LOCK_SCREEN);
    g_real_unlock = *(unlock_fn *)(rs + RS_UNLOCK_SCREEN);
    if (!g_real_lock || !g_real_unlock) {
        return;
    }
    *(lock_fn *)(rs + RS_LOCK_SCREEN) = batch_lock;
    *(unlock_fn *)(rs + RS_UNLOCK_SCREEN) = batch_unlock;
    g_rs = rs;
    g_batch.locked = 0;
}

void __cdecl lm_console_end(void)
{
    if (!g_rs) {
        return;
    }
    *(lock_fn *)(g_rs + RS_LOCK_SCREEN) = g_real_lock;
    *(unlock_fn *)(g_rs + RS_UNLOCK_SCREEN) = g_real_unlock;
    g_rs = NULL;
    if (g_batch.locked) {
        g_batch.locked = 0;
        g_real_unlock();             /* one dirty rectangle for the whole console */
    }
}

/* call sites: the lines (thiscall, two stack arguments, already pushed) and the input line (thiscall, none) */
extern void lm_console_lines_entry(void), lm_console_input_entry(void);
__asm__(".text\n"
        ".globl _lm_console_lines_entry\n"
        "_lm_console_lines_entry:\n"
        "    pushal\n"
        "    call _lm_console_begin\n"
        "    popal\n"
        "    movl $0x4172e8, %eax\n"      /* eax is free here: the call's result is what counts */
        "    jmp *%eax\n"
        ".globl _lm_console_input_entry\n"
        "_lm_console_input_entry:\n"
        "    movl $0x41635e, %eax\n"
        "    call *%eax\n"
        "    pushal\n"
        "    call _lm_console_end\n"
        "    popal\n"
        "    ret\n");

static void install_console(void)
{
    static const BYTE context[29] = {0x33, 0xff, 0x57, 0x8b, 0xcb, 0xff, 0xb3, 0xc4, 0x03, 0x00, 0x00, 0xe8, 0xa8,
                                     0xfc, 0xff, 0xff, 0x8d, 0x4b, 0x10, 0xe8, 0x16, 0xed, 0xff, 0xff, 0x5f, 0x5e,
                                     0x5b, 0xc9, 0xc3};
    static const BYTE alpha_one[4] = {0x00, 0x00, 0x80, 0x3f}, alpha_less[4] = {0x77, 0xbe, 0x7f, 0x3f};
    if (!lm_expect(0x417630u, context, sizeof context)
        || !lm_redirect_call(CONSOLE_LINES_CALL, CONSOLE_LINES, (const void *)lm_console_lines_entry)
        || !lm_redirect_call(CONSOLE_INPUT_CALL, CONSOLE_INPUT, (const void *)lm_console_input_entry)) {
        lm_log("framerate: the console's drawing (0x417630) differs, its text left alone");
    } else {
        lm_log("framerate: console text with one back buffer lock per frame");
    }
    lm_patch(CONSOLE_ALPHA_DEFAULT, alpha_one, alpha_less, sizeof alpha_less);
}

void lm_framerate_install(void)
{
    static const BYTE flip_head[7] = {0x83, 0x3d, 0x24, 0x49, 0x49, 0x00, 0x00};   /* cmp dword [0x494924],0 */
    QueryPerformanceFrequency(&g_pf);
    g_perf = lm_ini_int("Debug", "PerfLog", 0);
    if (g_perf) {
        lm_log("framerate: PerfLog on, a line every 5 seconds");
    }
    if (lm_ini_int("Performance", "TimerResolution", 1)) {
        timeBeginPeriod(1);          /* for the life of the process (Windows ends it at exit) */
        lm_log("framerate: timer resolution 1 ms");
        if (lm_ini_int("Performance", "PreciseLimiter", 1)) {
            install_limiter();
        }
    }
    g_fix_menus = lm_ini_int("Performance", "FixMenus", 1);
    g_fix_console = lm_ini_int("Performance", "FixConsole", 1);
    if (g_fix_menus) {
        lm_menufast_install();       /* the text, fills and screen grabs of the menus on the GPU */
    }
    if (g_fix_console) {
        install_console();
    }
    /* always: the voice HUD draws from it too */
    g_flip = (unsigned (__cdecl *)(unsigned))lm_detour(FLIP_SCREEN, flip_head, sizeof flip_head, (const void *)hk_flip);
    if (!g_flip) {
        lm_log("framerate: FlipScreen (%08x) differs, the menus present as they did", FLIP_SCREEN);
    } else if (g_fix_menus) {
        lm_log("framerate: menus present with one copy per frame");
    }
}
