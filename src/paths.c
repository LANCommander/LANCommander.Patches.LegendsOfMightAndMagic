/* The install folder and the user's save folder.
 *
 * Save folder: <Saved Games>\New World Computing\Legends of Might and Magic (Saved Games: %USERPROFILE%\Saved Games
 * unless the user moved it). [Saves] Folder= overrides it
 * (environment variables are expanded); [Saves] Redirect=0 turns the redirection off (lm_save_dir() is then "").
 * The [Saves] settings come from lommpatch.ini (or Config.yml) only: autoexec.cfg (settings.c), which can override
 * every other setting, lives in the folder they choose. */
#include <stdio.h>
#include <string.h>
#include "lommpatch.h"

static char g_game[MAX_PATH];
static char g_save[MAX_PATH];
static int g_save_done;              /* 1 = being worked out, 2 = done */

static void add_slash(char *s)
{
    size_t n = strlen(s);
    if (n && s[n - 1] != '\\' && n + 1 < MAX_PATH) {
        s[n] = '\\';
        s[n + 1] = '\0';
    }
}

void lm_make_dirs(const char *path)
{
    char tmp[MAX_PATH], *p;
    lstrcpynA(tmp, path, MAX_PATH);
    for (p = tmp + 3; *p; p++) {          /* skip "C:\" */
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
}

const char *lm_game_dir(void)
{
    if (!g_game[0]) {
        char *slash;
        GetModuleFileNameA(NULL, g_game, MAX_PATH);
        slash = strrchr(g_game, '\\');
        if (slash) {
            slash[1] = '\0';
        }
    }
    return g_game;
}

#define SAVE_SUBDIR "New World Computing\\Legends of Might and Magic"

/* the Saved Games folder (FOLDERID_SavedGames, Vista and later: looked up at run time), else %USERPROFILE%\Saved Games */
static int saved_games(char *out)
{
    static const GUID SAVED_GAMES = {0x4c5c32ff, 0xbb9d, 0x43b0, {0xb5, 0xb4, 0x2d, 0x72, 0xe5, 0x4e, 0xaa, 0xa4}};
    typedef HRESULT (WINAPI *known_folder_fn)(const GUID *, DWORD, HANDLE, PWSTR *);
    HMODULE shell = LoadLibraryA("shell32.dll");
    known_folder_fn known = shell ? (known_folder_fn)(void (*)(void))GetProcAddress(shell, "SHGetKnownFolderPath") : NULL;
    PWSTR wide = NULL;
    DWORD n;
    int ok = 0;
    if (known && SUCCEEDED(known(&SAVED_GAMES, 0, NULL, &wide)) && wide) {
        ok = WideCharToMultiByte(CP_ACP, 0, wide, -1, out, MAX_PATH - 64, NULL, NULL) > 0;
        CoTaskMemFree(wide);
    }
    if (!ok) {
        n = GetEnvironmentVariableA("USERPROFILE", out, MAX_PATH - 80);
        if (!n || n >= MAX_PATH - 80) {
            return 0;
        }
        add_slash(out);
        strcat(out, "Saved Games");
    }
    return 1;
}

static int default_folder(char *out)
{
    if (!saved_games(out)) {
        return 0;
    }
    add_slash(out);
    strcat(out, SAVE_SUBDIR);
    return 1;
}

/* an autoexec.cfg with key bindings in it (one written while the engine saw no keyboard or mouse has none) */
static int has_bindings(const char *path)
{
    char line[256];
    int found = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof line, f)) {
        found = !_strnicmp(line, "rangebind", 9);
    }
    fclose(f);
    return found;
}

/* patch 1.0.0 kept the save folder in %USERPROFILE%\New World Computing\Legends of Might and Magic: its
 * files are moved to the new one (those the new one has already stay where they are), and the folder is removed when
 * that leaves it empty. An autoexec.cfg without key bindings, written while the engine saw no keyboard or mouse
 * (input.c), is kept as autoexec.cfg.nobindings: the install folder's copy is used instead (saves.c). */
static void move_old_folder(const char *to)
{
    char old[MAX_PATH], from[MAX_PATH], dst[MAX_PATH], *slash;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD n = GetEnvironmentVariableA("USERPROFILE", old, MAX_PATH - 80);
    if (!n || n >= MAX_PATH - 80) {
        return;
    }
    add_slash(old);
    strcat(old, SAVE_SUBDIR "\\");
    if (!_stricmp(old, to)) {
        return;
    }
    snprintf(from, sizeof from, "%s*", old);
    if ((h = FindFirstFileA(from, &fd)) == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        snprintf(from, sizeof from, "%s%s", old, fd.cFileName);
        snprintf(dst, sizeof dst, "%s%s%s", to, fd.cFileName,
                 !_stricmp(fd.cFileName, "autoexec.cfg") && !has_bindings(from) ? ".nobindings" : "");
        if (MoveFileExA(from, dst, MOVEFILE_COPY_ALLOWED)) {
            lm_log("saves: moved %s to %s", from, dst);
        } else {
            lm_log("saves: %s left in the old save folder (%lu)", fd.cFileName, GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    old[strlen(old) - 1] = '\0';
    if (RemoveDirectoryA(old)) {
        lm_log("saves: removed the old save folder %s", old);
        if ((slash = strrchr(old, '\\'))) {
            *slash = '\0';
            RemoveDirectoryA(old);       /* New World Computing, if nothing else is in it */
        }
    }
}

static void find_save_dir(void)
{
    char folder[MAX_PATH], ovr[MAX_PATH];
    if (!lm_ini_int("Saves", "Redirect", 1)) {
        lm_log("saves: Redirect=0, files stay in the install folder");
        return;
    }
    ovr[0] = '\0';
    lm_ini_str("Saves", "Folder", ovr, sizeof ovr);
    if (ovr[0]) {
        char exp[MAX_PATH];
        DWORD n = ExpandEnvironmentStringsA(ovr, exp, sizeof exp);
        if (!n || n > sizeof exp || !GetFullPathNameA(exp, MAX_PATH - 48, folder, NULL)) {
            folder[0] = '\0';
        }
    } else if (!default_folder(folder)) {
        folder[0] = '\0';
    }
    if (!folder[0]) {
        lm_log("saves: no usable save folder, files stay in the install folder");
        return;
    }
    add_slash(folder);
    lm_make_dirs(folder);
    if (GetFileAttributesA(folder) == INVALID_FILE_ATTRIBUTES) {
        lm_log("saves: cannot create %s (%lu), files stay in the install folder", folder, GetLastError());
        return;
    }
    if (!ovr[0]) {
        move_old_folder(folder);
    }
    lstrcpynA(g_save, folder, MAX_PATH);
}

const char *lm_save_dir(void)
{
    if (!g_save_done) {
        g_save_done = 1;
        find_save_dir();
        g_save_done = 2;
    }
    return g_save;
}

const char *lm_save_dir_ready(void)
{
    return g_save_done == 1 ? NULL : lm_save_dir();
}
