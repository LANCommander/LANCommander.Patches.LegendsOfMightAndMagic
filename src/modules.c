/* The game's DLLs, as the engine loads them.
 *
 * lithtech.exe copies Data\cshell.dll and object.lto to %TEMP% under temporary names and LoadLibrary()s the copies
 * (0x4259e3 / 0x4253b5 through 0x401587), loads the renderer (d3d.ren) the same way, and the dedicated server
 * (LegendServ.exe) loads server.dll, which loads object.lto. All of them are built for 0x10000000 and are relocated,
 * so the patch addresses them as module base + RVA. They are recognised by their PE header (timestamp, size), not by
 * name, and the patch modules registered for each (lm_on_module) run right after LoadLibraryA returns, before the
 * engine calls into the DLL. LoadLibraryA / FreeLibrary are hooked in the exe's import table and in server.dll's. */
#include <string.h>
#include "lommpatch.h"

static const struct {
    const char *name;
    DWORD timestamp, size;
} KNOWN[LM_MOD_COUNT] = {
    [LM_MOD_CSHELL] = {"cshell.dll", 0x3b546e63u, 0xb9000u},
    [LM_MOD_OBJECT] = {"object.lto", 0x3b570de9u, 0x19f000u},
    [LM_MOD_RENDERER] = {"d3d.ren", 0x3b0c29d6u, 0xa4000u},
    [LM_MOD_SERVER] = {"server.dll", 0x3b0c631fu, 0xa3000u},
};

#define MAX_FNS 8
static lm_module_fn g_fns[LM_MOD_COUNT][MAX_FNS];
static int g_n_fns[LM_MOD_COUNT];
static HMODULE g_loaded[LM_MOD_COUNT];
static void (*g_unload[LM_MOD_COUNT][MAX_FNS])(void);
static int g_n_unload[LM_MOD_COUNT];

void lm_on_module_unload(int which, void (*fn)(void))
{
    if (which >= 0 && which < LM_MOD_COUNT && g_n_unload[which] < MAX_FNS) {
        g_unload[which][g_n_unload[which]++] = fn;
    }
}

static HMODULE (WINAPI *real_LoadLibraryA)(LPCSTR);
static BOOL (WINAPI *real_FreeLibrary)(HMODULE);

void lm_on_module(int which, lm_module_fn fn)
{
    if (which < 0 || which >= LM_MOD_COUNT || g_n_fns[which] >= MAX_FNS) {
        return;
    }
    g_fns[which][g_n_fns[which]++] = fn;
    if (g_loaded[which]) {
        fn(g_loaded[which]);         /* already there */
    }
}

HMODULE lm_game_module(int which)
{
    return which >= 0 && which < LM_MOD_COUNT ? g_loaded[which] : NULL;
}

static int identify(HMODULE m)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
    const IMAGE_NT_HEADERS32 *nt;
    int i;
    if (!m || !lm_readable(dos, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return -1;
    }
    nt = (const IMAGE_NT_HEADERS32 *)((const char *)m + dos->e_lfanew);
    if (!lm_readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return -1;
    }
    for (i = 0; i < LM_MOD_COUNT; i++) {
        if (nt->FileHeader.TimeDateStamp == KNOWN[i].timestamp && nt->OptionalHeader.SizeOfImage == KNOWN[i].size) {
            return i;
        }
    }
    return -1;
}

static void hook_loader(HMODULE m);

static HMODULE WINAPI hk_LoadLibraryA(LPCSTR name)
{
    HMODULE m = real_LoadLibraryA(name);
    int which = identify(m), i;
    if (which >= 0 && g_loaded[which] != m) {
        g_loaded[which] = m;
        lm_log("modules: %s loaded at %p (%s)", KNOWN[which].name, (void *)m, name ? name : "");
        if (which == LM_MOD_SERVER) {
            hook_loader(m);          /* it loads object.lto */
        }
        for (i = 0; i < g_n_fns[which]; i++) {
            g_fns[which][i](m);
        }
    }
    return m;
}

static BOOL WINAPI hk_FreeLibrary(HMODULE m)
{
    int i;
    for (i = 0; i < LM_MOD_COUNT; i++) {
        if (m && g_loaded[i] == m) {
            BOOL r = real_FreeLibrary(m);
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)m, &m)) {
                int k;
                lm_log("modules: %s unloaded", KNOWN[i].name);
                g_loaded[i] = NULL;
                for (k = 0; k < g_n_unload[i]; k++) {
                    g_unload[i][k]();
                }
            }
            return r;
        }
    }
    return real_FreeLibrary(m);
}

static void hook_loader(HMODULE m)
{
    void *old = lm_hook_import_in(m, "KERNEL32.dll", "LoadLibraryA", hk_LoadLibraryA);
    if (!real_LoadLibraryA) {
        real_LoadLibraryA = old ? old : (void *)LoadLibraryA;
    }
    old = lm_hook_import_in(m, "KERNEL32.dll", "FreeLibrary", hk_FreeLibrary);
    if (!real_FreeLibrary) {
        real_FreeLibrary = old ? old : (void *)FreeLibrary;
    }
}

void lm_modules_install(void)
{
    hook_loader(GetModuleHandleA(NULL));
}
