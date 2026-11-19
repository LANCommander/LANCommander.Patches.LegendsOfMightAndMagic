/* [Saves] Redirect: the files the game writes go to the save folder (paths.c, default
 * %USERPROFILE%\Saved Games\New World Computing\Legends of Might and Magic) instead of the install folder:
 *   autoexec.cfg   the settings and key bindings (read and written by lithtech.exe and cshell.dll)
 *   favorites.txt  the server browser's favourites, buddylist.txt its buddies, ROTATION.TXT the hosted map rotation
 *   ScreenshotN.bmp, legends.log, error.log
 * The game opens all of them by relative name (the working directory is the install folder) through
 * KERNEL32!CreateFileA / GetFileAttributesA / DeleteFileA, which are hooked in lithtech.exe's import table and in
 * cshell.dll's as the engine loads it. The first time, the install folder's copies are copied over, so nothing is
 * lost; after that the install folder's are not used. The registry settings are in registry.c. */
#include <stdio.h>
#include <string.h>
#include "lommpatch.h"

static const char *const FILES[] = {
    "autoexec.cfg", "favorites.txt", "buddylist.txt", "rotation.txt", "legends.log", "error.log",
};

static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static DWORD (WINAPI *real_GetFileAttributesA)(LPCSTR);
static BOOL (WINAPI *real_DeleteFileA)(LPCSTR);

static int is_screenshot(const char *name)
{
    size_t n = strlen(name);
    return n > 14 && !_strnicmp(name, "screenshot", 10) && !_stricmp(name + n - 4, ".bmp");
}

/* the save folder's path for a file of the game's in out, if it is one */
static int redirect(const char *path, char *out, int size)
{
    char full[MAX_PATH], *name;
    const char *game = lm_game_dir();
    size_t i, dir_len;
    if (!path || !path[0] || !lm_save_dir()[0] || !GetFullPathNameA(path, MAX_PATH, full, &name) || !name) {
        return 0;
    }
    dir_len = (size_t)(name - full);
    if (dir_len != strlen(game) || _strnicmp(full, game, dir_len)) {
        return 0;                    /* only the install folder itself */
    }
    for (i = 0; i < LM_COUNT(FILES) && _stricmp(name, FILES[i]); i++) {
    }
    if (i == LM_COUNT(FILES) && !is_screenshot(name)) {
        return 0;
    }
    return snprintf(out, size, "%s%s", lm_save_dir(), name) < size;
}

static HANDLE WINAPI hk_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD how,
                                    DWORD flags, HANDLE templ)
{
    char to[MAX_PATH];
    return real_CreateFileA(redirect(path, to, sizeof to) ? to : path, access, share, sa, how, flags, templ);
}

static DWORD WINAPI hk_GetFileAttributesA(LPCSTR path)
{
    char to[MAX_PATH];
    return real_GetFileAttributesA(redirect(path, to, sizeof to) ? to : path);
}

static BOOL WINAPI hk_DeleteFileA(LPCSTR path)
{
    char to[MAX_PATH];
    return real_DeleteFileA(redirect(path, to, sizeof to) ? to : path);
}

static void hook(HMODULE m, const char *what)
{
    void *a = lm_hook_import_in(m, "KERNEL32.dll", "CreateFileA", hk_CreateFileA);
    void *b = lm_hook_import_in(m, "KERNEL32.dll", "GetFileAttributesA", hk_GetFileAttributesA);
    void *c = lm_hook_import_in(m, "KERNEL32.dll", "DeleteFileA", hk_DeleteFileA);
    if (!real_CreateFileA) real_CreateFileA = a ? a : (void *)CreateFileA;
    if (!real_GetFileAttributesA) real_GetFileAttributesA = b ? b : (void *)GetFileAttributesA;
    if (!real_DeleteFileA) real_DeleteFileA = c ? c : (void *)DeleteFileA;
    lm_log("saves: %s's files go to %s", what, lm_save_dir());
}

static void on_cshell(HMODULE m)
{
    hook(m, "cshell.dll");
}

/* the install folder's files, copied to the save folder the first time */
static void migrate(void)
{
    char from[MAX_PATH], to[MAX_PATH];
    size_t i;
    for (i = 0; i < LM_COUNT(FILES); i++) {
        snprintf(from, sizeof from, "%s%s", lm_game_dir(), FILES[i]);
        snprintf(to, sizeof to, "%s%s", lm_save_dir(), FILES[i]);
        if (i >= 4 || GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES
            || GetFileAttributesA(from) == INVALID_FILE_ATTRIBUTES) {
            continue;                /* the logs start afresh */
        }
        if (CopyFileA(from, to, TRUE)) {
            lm_log("saves: copied %s to the save folder", FILES[i]);
        } else {
            lm_log("saves: cannot copy %s to %s (%lu)", from, to, GetLastError());
        }
    }
}

/* once, before anything reads the save folder's files (settings.c reads autoexec.cfg before the patch installs) */
void lm_saves_prepare(void)
{
    static LONG done;
    if (lm_process() == LM_PROC_CLIENT && lm_save_dir()[0] && !InterlockedExchange(&done, 1)) {
        migrate();
    }
}

void lm_saves_install(void)
{
    if (!lm_save_dir()[0]) {
        return;
    }
    lm_saves_prepare();
    hook(GetModuleHandleA(NULL), "lithtech.exe");
    lm_on_module(LM_MOD_CSHELL, on_cshell);
}
