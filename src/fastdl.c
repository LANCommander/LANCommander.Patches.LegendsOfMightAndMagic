/* [FastDL] Enabled: the custom worlds the game lacks, downloaded from the game server's FastDL server as the game joins
 * it, by the LANCommander Interposer's FastDL.
 *
 * - The game server (running this patch, voice_server.c) says where its FastDL server is in its GameSpy query
 *   replies: \fastdlport\, with \fastdlhost\ (else the game server's own host) and \fastdlpath\ (else "/").
 * - Joining (voice_game.c: cshell's join, before the engine connects), this module asks the server's query port for
 *   that and hands the URL to the Interposer (InterposerSetFastDLBaseUrl). It has to be then: right after the
 *   connection the server sends its file list, and the client looks up each name in its resource trees at once,
 *   keeping only those it finds. The server's FastDL server always wins: over [FastDL] Url (used only when the
 *   server names none) and over the Interposer's own FastDL.BaseUrl.
 * - Custom worlds are loose files: Data\Worlds\<name>.dat and the folder Data\Worlds\<name>Files\. The engine looks
 *   for a file with FindFirstFileA (the CRT's _findfirst, lithtech.exe's import) before it opens it, and the
 *   Interposer's FastDL acts on opening a file or asking for its attributes, not on FindFirstFile. So a
 *   FindFirstFileA of lithtech.exe that finds nothing under Data\Worlds asks GetFileAttributesA (which downloads the
 *   file if the FastDL server has it) and then looks again. The embedded Interposer's Config.yml
 *   (interposer_config.c) downloads into the game folder itself (UseDownloadDirectory: false), where the engine looks.
 * - The Interposer's Config.yml decides the rest (FastDL.Enabled, FastDL.Paths). */
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"

typedef BOOL (*set_url_fn)(const wchar_t *base_url);

static char g_server[96];            /* the game server whose FastDL server the Interposer was given */
static int g_enabled, g_active;

static set_url_fn interposer_set_url(void)
{
    static int tried;
    static set_url_fn set;
    if (tried) {
        return set;
    }
    tried = 1;
    if (!lm_interposer()) {
        lm_log("fastdl: no LANCommander Interposer, no FastDL");
        return NULL;
    }
    if (!(set = (set_url_fn)(void (*)(void))GetProcAddress(lm_interposer(), "InterposerSetFastDLBaseUrl"))) {
        lm_log("fastdl: the LANCommander Interposer in the game has no InterposerSetFastDLBaseUrl; update it for FastDL");
    }
    return set;
}

/* the FastDL server the game server's query reply names, as a URL in url ("" for none) */
static void server_url(const char *reply, const char *game_host, char *url, int size)
{
    char port[16], host[MAX_PATH], path[MAX_PATH];
    int len = (int)strlen(reply), p;
    url[0] = '\0';
    if (!lm_query_value(reply, len, "fastdlport", port, sizeof port) || (p = atoi(port)) <= 0 || p > 65535) {
        return;
    }
    if (!lm_query_value(reply, len, "fastdlhost", host, sizeof host) || !host[0]) {
        lstrcpynA(host, game_host, sizeof host);
    }
    if (!lm_query_value(reply, len, "fastdlpath", path, sizeof path) || !path[0]) {
        strcpy(path, "/");
    }
    snprintf(url, size, "http://%s:%d%s%s", host, p, path[0] == '/' ? "" : "/", path);
}

/* on the game thread, as the game joins host (voice_game.c) */
void lm_fastdl_join(const char *host, WORD game_port, WORD query_port)
{
    char address[96], reply[2048], url[2 * MAX_PATH], fallback[MAX_PATH];
    wchar_t wide[2 * MAX_PATH];
    WORD ports[8];
    set_url_fn set;
    int r, n;
    if (!g_enabled || !host[0] || !(set = interposer_set_url())) {
        return;
    }
    snprintf(address, sizeof address, "%s:%u", host, game_port);
    if (!strcmp(address, g_server)) {
        return;                      /* the same server again: the Interposer has its FastDL server */
    }
    url[0] = '\0';
    g_server[0] = '\0';
    n = lm_voice_query_ports(game_port, ports, 8);
    if (query_port) {
        ports[0] = query_port;       /* the browser knows it: ask that one first */
    }
    if ((r = lm_query_info(host, ports, n, 0, reply, sizeof reply)) < 0) {
        lm_log("fastdl: cannot resolve the game server %s", host);
    } else if (!r) {
        lm_log("fastdl: the game server %s does not answer its queries", address);
    } else {
        lstrcpynA(g_server, address, sizeof g_server);
        server_url(reply, host, url, sizeof url);
        if (!url[0]) {
            lm_log("fastdl: the game server %s has no FastDL server", address);
        }
    }
    if (!url[0] && lm_ini_str("FastDL", "Url", fallback, sizeof fallback)) {
        lstrcpynA(url, fallback, sizeof url);
        lm_log("fastdl: using [FastDL] Url instead");
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, LM_COUNT(wide))) {
        wide[0] = L'\0';
    }
    if (set(wide)) {
        g_active = 1;                /* the Interposer has a FastDL server: from the game server, ours, or its own */
        lm_log(url[0] ? "fastdl: joining %s, missing worlds come from %s"
                      : "fastdl: joining %s, missing worlds come from the Interposer's own FastDL server, if any%s",
               address, url);
    } else if (url[0]) {
        lm_log("fastdl: the Interposer did not take %s: FastDL is off in its Config.yml (FastDL.Enabled, "
               "FastDL.Paths), or it is not an http:// URL", url);
    }
}

/* ---------------------------------------------------------------- the engine's look for a file */

static HANDLE (WINAPI *real_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);

static int in_worlds(const char *path)
{
    char full[MAX_PATH], prefix[MAX_PATH];
    snprintf(prefix, sizeof prefix, "%sData\\Worlds\\", lm_game_dir());
    return GetFullPathNameA(path, MAX_PATH, full, NULL) && !_strnicmp(full, prefix, strlen(prefix))
           && !strchr(full + strlen(prefix), '*') && !strchr(full + strlen(prefix), '?');
}

static HANDLE WINAPI hk_FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data)
{
    HANDLE h = real_FindFirstFileA(path, data);
    if (h == INVALID_HANDLE_VALUE && g_active && path && in_worlds(path)) {
        DWORD err = GetLastError();
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {   /* the Interposer downloaded it */
            lm_log("fastdl: %s downloaded", path);
            return real_FindFirstFileA(path, data);
        }
        SetLastError(err);
    }
    return h;
}

void lm_fastdl_install(void)
{
    if (!(g_enabled = lm_ini_int("FastDL", "Enabled", 1))) {
        lm_log("fastdl: [FastDL] Enabled=0, no FastDL");
        return;
    }
    lm_game_install();
    real_FindFirstFileA = lm_hook_import("KERNEL32.dll", "FindFirstFileA", hk_FindFirstFileA);
    if (!real_FindFirstFileA) {
        real_FindFirstFileA = FindFirstFileA;
    }
}
