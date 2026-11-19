#ifndef LOMMPATCH_H
#define LOMMPATCH_H
#include <windows.h>
#include <stdint.h>

#ifndef LM_VERSION
#define LM_VERSION "dev"             /* build.sh passes the release tag */
#endif

/* which program the patch is in */
enum { LM_PROC_NONE, LM_PROC_CLIENT, LM_PROC_DEDICATED };
int lm_process(void);                /* lithtech.exe (the game) or LegendServ.exe (the dedicated server) */

int lm_patch_start(void);            /* exe check + Enabled + not loaded twice; 1 = go ahead and install the modules */
void lm_install(void);               /* installs the patch modules */
void lm_log(const char *fmt, ...);
void lm_standalone_init(HINSTANCE dll);   /* proxy / ASI DllMain: settings and log next to the DLL, then the patches */
void lm_standalone_detach(BOOL process_terminating);   /* proxy / ASI DllMain: the embedded Interposer stops */

/* where settings and log lines go: lommpatch.ini / lommpatch.log (proxy, ASI), or callbacks (Interposer plugin) */
void lm_set_files(const char *ini, const char *log);
const char *lm_log_file(void);       /* "" with the Interposer backend */
void lm_set_backend(int (*config_int)(const char *section, const char *key, int def), void (*log_line)(const char *line));
void lm_set_string_backend(int (*config_str)(const char *section, const char *key, char *out, int size));
HMODULE lm_module(void);             /* this DLL */
void lm_set_module(HMODULE dll);

/* memory helpers. lithtech.exe / LegendServ.exe addresses are VAs (image base 0x400000, no relocations); the game's
 * DLLs (cshell.dll, object.lto, d3d.ren, server.dll) can be relocated, so their addresses are module base + RVA. */
int lm_write(uintptr_t addr, const void *bytes, size_t len);
int lm_expect(uintptr_t addr, const void *bytes, size_t len);  /* 1 if memory matches */
int lm_patch(uintptr_t addr, const void *expect, const void *bytes, size_t len);
int lm_jmp(uintptr_t from, const void *to);   /* E9 rel32 at from */
int lm_call(uintptr_t from, const void *to);  /* E8 rel32 at from */
int lm_redirect_call(uintptr_t site, uintptr_t old_target, const void *to);   /* E8 rel32 at site, if it calls old_target */
/* a detour: the len bytes at fn (whole instructions, no relative jumps or calls) are checked against prologue, copied
 * into a trampoline that goes on at fn + len, and replaced by a jump to hook. Returns the trampoline (the original
 * function, called like it), NULL if the bytes differ. */
void *lm_detour(uintptr_t fn, const void *prologue, size_t len, const void *hook);
int lm_hook_slot(uintptr_t slot, uintptr_t expect, const void *hook);   /* a function table entry, if it holds expect */
int lm_readable(const void *p, size_t n);    /* committed, readable memory */

/* settings: [section] key as the player's console variable (autoexec.cfg, settings.c), else lommpatch.ini (or
 * Plugins.LegendsOfMightAndMagic.<section>.<key> in the Interposer's Config.yml) */
int lm_ini_int(const char *section, const char *key, int def);
int lm_ini_str(const char *section, const char *key, char *out, int size);   /* "" if missing; 1 if not empty */
int lm_ini_default(const char *section, const char *key, char *out, int size);   /* lommpatch.ini / Config.yml only */
const char *lm_ini_file(void);       /* lommpatch.ini (next to lithtech.exe with the Interposer plugin) */

/* settings.c: console variables in autoexec.cfg (the running engine's, once it is up) */
int lm_settings_lookup(const char *section, const char *key, char *out, int size);
int lm_setting_get(const char *cvar, char *out, int size);   /* 1 if the variable exists */
int lm_setting_int(const char *cvar, int def);
void lm_setting_set(const char *cvar, const char *value);   /* the engine's variable and autoexec.cfg */
void lm_setting_set_int(const char *cvar, int value);

void *lm_hook_import(const char *dll, const char *name, void *replacement);  /* in the exe; returns the old target */
void *lm_hook_import_in(HMODULE module, const char *dll, const char *name, void *replacement);
void *lm_hook_import_ordinal(HMODULE module, const char *dll, WORD ordinal, void *replacement);

/* paths.c: the install folder and the user's save folder (both with a trailing backslash) */
const char *lm_game_dir(void);
const char *lm_save_dir(void);       /* "" when saves are not redirected */
const char *lm_save_dir_ready(void); /* lm_save_dir(), or NULL from within its own setting lookups */
void lm_make_dirs(const char *path); /* creates every missing parent folder of path (and path itself if it ends in \) */

/* modules.c: the game's DLLs, as the engine loads them (cshell.dll and object.lto are copied to %TEMP% under a temp
 * name first). Each patch module registers for the DLLs it patches; fn runs right after LoadLibrary, before the
 * engine calls into the DLL. */
enum { LM_MOD_CSHELL, LM_MOD_OBJECT, LM_MOD_RENDERER, LM_MOD_SERVER, LM_MOD_COUNT };
typedef void (*lm_module_fn)(HMODULE module);
void lm_on_module(int which, lm_module_fn fn);
void lm_on_module_unload(int which, void (*fn)(void));   /* after the engine freed it */
/* framerate.c: fn runs in FlipScreen, before the frame is shown (draws on top of everything) */
void lm_on_flip(void (*fn)(void));
/* the engine's fonts and text (lithtech.exe, cdecl; through ui.c's scaling when it is on) */
void *lm_engine_font(const char *face, int height, int bold);
void lm_engine_text(int x, int y, int w, int h, void *font, const char *text, DWORD rgb);
HMODULE lm_game_module(int which);   /* NULL until loaded (and after it is unloaded) */
#define LM_RVA(mod, rva) ((uintptr_t)(mod) + (uintptr_t)(rva))

/* patch modules */
void lm_modules_install(void);       /* modules.c: watches the engine load cshell.dll, object.lto, d3d.ren, server.dll */
void lm_crashlog_install(void);      /* crashlog.c: [Debug] CrashLog=1 logs crashes with a call chain */
void lm_saves_install(void);         /* saves.c: autoexec.cfg, favourites, buddies, screenshots in the save folder */
void lm_saves_prepare(void);         /* saves.c: the install folder's files copied to the save folder the first time */
void lm_registry_install(void);      /* registry.c: the game's registry key in a file in the save folder */
void lm_input_install(void);         /* input.c: DirectInput 7 device types for the engine (keyboard, mouse bindings) */
void lm_widescreen_install(void);    /* widescreen.c: Hor+ field of view */
void lm_window_install(void);        /* window.c: the window over the monitor in fullscreen; borderless fullscreen */
int lm_borderless(void);
void lm_window_map_mouse(int *x, int *y);   /* borderless: a point in the window to one in the frame */
void lm_ui_install(void);            /* ui.c: menus scaled to fit the screen, HUD scale */
void lm_framerate_install(void);     /* framerate.c: the menus and the console without frame rate drops */
void lm_menufast_install(void);      /* menufast.c: menu text, fills and grabs on the GPU */
/* framerate.c: [Debug] PerfLog's counters, logged and reset every 5 seconds (game thread) */
typedef struct {
    unsigned frames, locks, blits, slow_blits, text_hits, text_new, text_soft, fills, flags_or, optimized;
    LONGLONG frame_ticks, worst_ticks, present_ticks, limiter_ticks, lock_ticks, blit_ticks;
} lm_perf_counters;
extern lm_perf_counters lm_perf;
int lm_fast_fill(void *rect, DWORD color);   /* menufast.c: FillRect onto the screen on the GPU; 1 if done */
/* ui.c: the engine's 2-D functions without ui.c's scaling, and what ui.c hooks */
int lm_raw_draw(void *dest, void *src, void *src_rect, int x, int y);
int lm_raw_draw_t(void *dest, void *src, void *src_rect, int x, int y, DWORD key);
int lm_raw_scale(void *dest, void *src, void *dst_rect, void *src_rect);
int lm_ui_has_fill(void);
int lm_ui_marks_grabs(void);
void lm_voice_install(void);         /* voice.c: voice chat (the Interposer's) in the game */
void lm_voice_menu_install(void);    /* voice_menu.c: Options > Sound > Voice chat */
void lm_voice_set_interposer(HMODULE interposer);   /* the Interposer the patch uses: the plugin build's, or its own */
HMODULE lm_interposer(void);
void lm_voice_server_install(void);  /* voice_server_win.c: the voice server and the query replies in the game server */
void lm_fastdl_install(void);        /* fastdl.c: the game server's FastDL server, handed to the Interposer on joining */
void lm_game_install(void);          /* voice_game.c: the join, the frame and the HUD lines of the client shell */
void lm_presence_install(void);      /* presence.c: rich presence (Discord, the LANCommander launcher) */
void lm_interposer_write_config(HMODULE dll);   /* interposer_config.c: the embedded Interposer's Config.yml */

#define LM_ADDR(a) ((void *)(uintptr_t)(a))
#define LM_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif
