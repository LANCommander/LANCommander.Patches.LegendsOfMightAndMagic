/* What voice chat and FastDL need from the game client (cshell.dll RVAs, as it is relocated):
 * - The join: every way of joining a server (the browser, favourites, join by address, +IpAddress on the command
 *   line) goes through one cshell function, RVA 0x59731 (thiscall(result, ServerInfo *si, void *connect_data, int
 *   len), ret 0xc). ServerInfo: +0x04 game port, +0x08 the host ("hostaddr"), +0x3c the GameSpy query port. It is
 *   detoured (its first instruction, mov eax, imm32) to remember the server and, before the engine connects, to let
 *   fastdl.c tell the Interposer where the server's FastDL server is (the server's file list arrives right after the
 *   connection, and a world missing then cannot be loaded for that connection).
 * - The local player: g_pLTClient (cshell+0xa42f8) +0x10 GetLocalClientID(uint32 *) (0 = LT_OK, else not
 *   connected), +0x0c GetGameMode(int *) (2 = hosting); the name the server knows is the connect data's
 *   (cshell+0xa4858, char[0x19]). The voice server's ids are client id + 1.
 * - The frame: IClientShell::Update (RVA 0x633ee, no arguments), its vtable slot RVA 0x8fe70.
 * - The HUD: a few lines of text drawn in FlipScreen, on top of the frame, with the engine's own text functions. */
#include <stdio.h>
#include <string.h>
#include "voice.h"

#define CS_JOIN           0x59731u
#define CS_JOIN_SEH       0x810c4u   /* the mov eax, imm32's operand (relocated) */
#define CS_LTCLIENT       0xa42f8u
#define CS_CONNECT_NAME   0xa4858u
#define CS_UPDATE_SLOT    0x8fe70u
#define CS_UPDATE         0x633eeu
#define LTC_GET_GAME_MODE 0x0c
#define LTC_LOCAL_ID      0x10
#define GAMEMODE_HOST     2

void lm_fastdl_join(const char *host, WORD game_port, WORD query_port);   /* fastdl.c */

static uintptr_t g_cs;
static char g_host[64];
static WORD g_game_port, g_query_port;

static void *ltclient(void)
{
    void *c = g_cs ? *(void **)(g_cs + CS_LTCLIENT) : NULL;
    return lm_readable(c, 0x20) ? c : NULL;
}

int lm_game_local_player(char *name, int size, int *id, int *team)
{
    void *c = ltclient();
    uint32_t cid = 0;
    const char *n;
    if (!c || ((int (__cdecl *)(uint32_t *))(*(void **)((BYTE *)c + LTC_LOCAL_ID)))(&cid) != 0) {
        return 0;
    }
    n = (const char *)(g_cs + CS_CONNECT_NAME);
    lstrcpynA(name, n, size < 0x1a ? size : 0x1a);
    *id = (int)cid;
    *team = 0;                       /* the voice server says (the team radio's group) */
    return name[0] != '\0';
}

int lm_game_mode(void)
{
    void *c = ltclient();
    int mode = 5;
    if (c) {
        ((int (__cdecl *)(int *))(*(void **)((BYTE *)c + LTC_GET_GAME_MODE)))(&mode);
    }
    return mode;
}

WORD lm_game_query_port(void)
{
    return g_query_port;
}

int lm_game_server(char *host, int size, WORD *port)
{
    if (lm_game_mode() == GAMEMODE_HOST) {
        lstrcpynA(host, "127.0.0.1", size);   /* this game hosts the server (and its voice server) */
        *port = 0;
        return 1;
    }
    if (!g_host[0]) {
        return 0;
    }
    lstrcpynA(host, g_host, size);
    *port = g_game_port;
    return 1;
}

int lm_voice_query_ports(WORD game_port, WORD *ports, int max)
{
    char text[128];
    int n = 0;
    if (lm_ini_str("Network", "QueryPorts", text, sizeof text) && (n = lm_voice_ports(text, ports, max)) > 0) {
        return n;
    }
    /* the query port the browser knew, the default one, the one after the game port */
    if (g_query_port && n < max) ports[n++] = g_query_port;
    if (g_query_port != 29801 && n < max) ports[n++] = 29801;
    if (game_port && game_port + 1 != 29801 && game_port + 1 != g_query_port && n < max) ports[n++] = game_port + 1;
    return n;
}

/* ---------------------------------------------------------------- the join */

/* on the game thread, before the engine connects */
void __cdecl lm_game_join(const BYTE *si)
{
    if (!lm_readable(si, 0x40)) {
        return;
    }
    lstrcpynA(g_host, (const char *)(si + 8), sizeof g_host);
    g_game_port = (WORD)*(const int *)(si + 4);
    g_query_port = (WORD)*(const int *)(si + 0x3c);
    lm_log("game: joining %s (game port %u, query port %u)", g_host, g_game_port, g_query_port);
    lm_fastdl_join(g_host, g_game_port, g_query_port);
}

/* thiscall(ecx, si, data, len): [esp+4] = si */
extern void lm_game_join_entry(void);
__asm__(".text\n"
        ".globl _lm_game_join_entry\n"
        "_lm_game_join_entry:\n"
        "    pushal\n"
        "    pushl 36(%esp)\n"           /* si: past the 8 registers and the return address */
        "    call _lm_game_join\n"
        "    addl $4, %esp\n"
        "    popal\n"
        "    jmp *_g_join_tramp\n");
void *g_join_tramp;

/* ---------------------------------------------------------------- the frame */

static void (*g_frame_fns[4])(void);
static int g_n_frame_fns;
static void (__attribute__((thiscall)) *g_update)(void *self);

void lm_game_on_frame(void (*fn)(void))
{
    if (g_n_frame_fns < (int)LM_COUNT(g_frame_fns)) {
        g_frame_fns[g_n_frame_fns++] = fn;
    }
}

static void __attribute__((thiscall)) hk_update(void *self)
{
    int i;
    g_update(self);
    for (i = 0; i < g_n_frame_fns; i++) {
        g_frame_fns[i]();
    }
}

/* ---------------------------------------------------------------- the HUD */

#define HUD_LINES 13
static char g_lines[HUD_LINES][96];
static int g_n_lines;
static void *g_font;

void lm_game_hud_lines(const char *const *lines, int n)
{
    int i;
    g_n_lines = n < HUD_LINES ? n : HUD_LINES;
    for (i = 0; i < g_n_lines; i++) {
        lstrcpynA(g_lines[i], lines[i], sizeof g_lines[i]);
    }
}

static void draw_hud(void)
{
    int i;
    if (!g_n_lines || !ltclient()) {
        return;
    }
    if (!g_font && !(g_font = lm_engine_font("Arial", 14, 1))) {
        return;
    }
    /* left edge, a third of the way down (design pixels: 640x480 scaled like the HUD) */
    for (i = 0; i < g_n_lines; i++) {
        lm_engine_text(8, 150 + i * 16, 300, 16, g_font, g_lines[i], i == 0 && g_lines[0][0] == '*' ? 0x40ff40 : 0xffffff);
    }
}

/* ---------------------------------------------------------------- install */

static void on_cshell(HMODULE m)
{
    uintptr_t cs = (uintptr_t)m;
    BYTE head[5];
    uint32_t seh = (uint32_t)(cs + CS_JOIN_SEH);
    g_cs = cs;
    g_font = NULL;                   /* the engine's fonts went with the old client shell */
    head[0] = 0xb8;
    memcpy(head + 1, &seh, 4);
    if (!(g_join_tramp = lm_detour(cs + CS_JOIN, head, sizeof head, (const void *)lm_game_join_entry))) {
        lm_log("game: cshell's join (RVA %x) differs: the voice server is found from the game's traffic, no FastDL",
               CS_JOIN);
    }
    g_update = (void (__attribute__((thiscall)) *)(void *))(cs + CS_UPDATE);
    if (!lm_hook_slot(cs + CS_UPDATE_SLOT, cs + CS_UPDATE, (const void *)hk_update)) {
        lm_log("game: cshell's Update slot differs, no voice chat");
    }
}

static int g_installed;

void lm_game_install(void)
{
    if (g_installed++) {
        return;
    }
    lm_on_module(LM_MOD_CSHELL, on_cshell);
    lm_on_flip(draw_hud);
}
