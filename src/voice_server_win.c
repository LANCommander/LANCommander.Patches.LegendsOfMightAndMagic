/* The game server's side of voice chat and FastDL, in object.lto (the server shell; RVAs, as it is relocated). It runs
 * in lithtech.exe while the game hosts (a listen server) and in LegendServ.exe (the dedicated server, through
 * server.dll); object.lto is loaded from a temporary copy each time a server starts and freed when it stops.
 * - The frame: IServerShell::Update (RVA 0xb6a19, thiscall(this, float), ret 4) through its vtable slot RVA 0x10e7bc.
 *   It also answers the GameSpy queries, so everything here runs on the server's thread.
 * - The query replies: the stock GameSpy qr SDK (gamename legendsmm) sends them through object.lto's WSOCK32 sendto
 *   (by ordinal, 20); the hook adds \commsport\ and the FastDL keys before the last packet's \final\ (voice_server.c).
 *   Heartbeats have no \final\ and go out as they were.
 * - The players: g_pLTServer (object.lto+0x174b60), cdecl members +0xcc GetNextClient(prev), +0xe0 GetClientID,
 *   +0xf4 GetClientUserData (the CPlayerObj), +0x1a0 GetObjectPos(HOBJECT, LTVector *). The game's player records
 *   (the team manager at object.lto+0x174c38: count +0x284, 0x28-byte records from +4: name +0, HCLIENT +0x1c, class
 *   +0x27; game type +0x28c, 6 = deathmatch) give the name and the team (from the class: 0x34/36/37/3b Evil,
 *   0x35/38/39/3a Good, 0x3c..0x43 as 8 lower). CPlayerObj: +0x08 HOBJECT, +0x25c dead, +0x25d dying, +0x108c
 *   spectator, +0x75c yaw (radians; forward = (sin yaw, 0, cos yaw)). */
#include <winsock2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lommpatch.h"
#include "voice_server.h"

#define LTO_SERVER        0x174b60u
#define LTO_TEAMS         0x174c38u
#define LTO_UPDATE        0xb6a19u
#define LTO_UPDATE_SLOT   0x10e7bcu
#define SRV_NEXT_CLIENT   0xcc
#define SRV_CLIENT_ID     0xe0
#define SRV_USER_DATA     0xf4
#define SRV_OBJECT_POS    0x1a0
#define TM_COUNT          0x284
#define TM_GAME_TYPE      0x28c
#define TM_RECORD         0x28
#define GAME_DEATHMATCH   6

static uintptr_t g_lto;
static int g_voice, g_fastdl;

void *lm_server_symbol(const char *name)
{
    return lm_interposer() ? (void *)GetProcAddress(lm_interposer(), name) : NULL;
}

uint64_t lm_server_now_ms(void)
{
    return GetTickCount64();
}

const char *lm_server_team_name(int team)
{
    return team == 1 ? "Evil" : team == 2 ? "Good" : "";
}

/* the class's team: 1 Evil, 2 Good, 0 none */
static int team_of_class(int c)
{
    if (c >= 0x3c && c <= 0x43) {
        c -= 8;
    }
    switch (c) {
    case 0x34: case 0x36: case 0x37: case 0x3b:
        return 1;
    case 0x35: case 0x38: case 0x39: case 0x3a:
        return 2;
    default:
        return 0;
    }
}

int lm_server_read_players(lm_server_player *out, int capacity)
{
    BYTE *srv, *tm;
    void *(__cdecl *next)(void *);
    uint32_t (__cdecl *client_id)(void *);
    BYTE *(__cdecl *user_data)(void *);
    void (__cdecl *object_pos)(void *, float *);
    void *hc;
    int n = 0, count, deathmatch, i;
    if (!g_lto) {
        return -1;
    }
    srv = *(BYTE **)(g_lto + LTO_SERVER);
    tm = (BYTE *)(g_lto + LTO_TEAMS);
    if (!lm_readable(srv, SRV_OBJECT_POS + 4) || (count = *(int *)(tm + TM_COUNT)) < 0 || count > 16) {
        return -1;
    }
    next = *(void *(__cdecl **)(void *))(srv + SRV_NEXT_CLIENT);
    client_id = *(uint32_t (__cdecl **)(void *))(srv + SRV_CLIENT_ID);
    user_data = *(BYTE *(__cdecl **)(void *))(srv + SRV_USER_DATA);
    object_pos = *(void (__cdecl **)(void *, float *))(srv + SRV_OBJECT_POS);
    if (!next || !client_id || !user_data || !object_pos) {
        return -1;
    }
    deathmatch = *(int *)(tm + TM_GAME_TYPE) == GAME_DEATHMATCH;
    for (hc = next(NULL); hc && n < capacity; hc = next(hc)) {
        lm_server_player *p = &out[n];
        const BYTE *rec = NULL;
        BYTE *player;
        memset(p, 0, sizeof *p);
        p->id = (int)client_id(hc);
        for (i = 0; i < count; i++) {
            const BYTE *r = tm + 4 + i * TM_RECORD;
            if (*(void *const *)(r + 0x1c) == hc) {
                rec = r;
                break;
            }
        }
        if (!rec || p->id < 0 || p->id > 255) {
            continue;                /* not in the game yet */
        }
        lstrcpynA(p->name, (const char *)rec, 0x1c < sizeof p->name ? 0x1c : sizeof p->name);
        p->team = deathmatch ? 0 : team_of_class(rec[0x27]);
        player = user_data(hc);
        if (lm_readable(player, 0x1090) && lm_readable(player + 0x75c, 4)) {
            float yaw = *(float *)(player + 0x75c);
            p->alive = !player[0x25c] && !player[0x25d] && !player[0x108c];
            object_pos(*(void **)(player + 8), p->position);
            p->has_position = 1;
            p->forward[0] = sinf(yaw);
            p->forward[1] = 0.0f;
            p->forward[2] = cosf(yaw);
        }
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------- hooks */

static void (__attribute__((thiscall)) *g_update)(void *self, float dt);

static void __attribute__((thiscall)) hk_update(void *self, float dt)
{
    g_update(self, dt);
    if (g_voice) {
        lm_server_tick();
    }
}

static int (WINAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);

static int WINAPI hk_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    char out[4096];
    int n = lm_server_query_reply(buf, len, out, sizeof out);
    if (n > 0) {
        int r = real_sendto(s, out, n, flags, to, tolen);
        return r == n ? len : r;     /* what the game sent, as far as it knows */
    }
    return real_sendto(s, buf, len, flags, to, tolen);
}

static void on_object(HMODULE m)
{
    void *old;
    g_lto = (uintptr_t)m;
    g_update = (void (__attribute__((thiscall)) *)(void *, float))(g_lto + LTO_UPDATE);
    if (!lm_hook_slot(g_lto + LTO_UPDATE_SLOT, g_lto + LTO_UPDATE, (const void *)hk_update)) {
        lm_log("voice server: object.lto's Update slot differs, no voice server");
    }
    old = lm_hook_import_ordinal(m, "WSOCK32.dll", 20, hk_sendto);
    if (!old) {
        old = lm_hook_import_in(m, "WSOCK32.dll", "sendto", hk_sendto);
    }
    if (old) {
        real_sendto = old;
    } else {
        lm_log("voice server: object.lto's sendto is not hooked; the query replies do not advertise voice or FastDL");
    }
    lm_log("voice server: hooked the server shell (voice %s, FastDL %s)", g_voice ? "on" : "off",
           g_fastdl ? "advertised" : "not advertised");
}

static void on_object_unload(void)
{
    g_lto = 0;
    lm_server_stop();
}

void lm_voice_server_install(void)
{
    g_voice = lm_ini_int("Server", "VoiceServer", 1) && lm_interposer();
    if (lm_ini_int("Server", "VoiceServer", 1) && !lm_interposer()) {
        lm_log("voice server: no LANCommander Interposer, no voice server");
    }
    g_fastdl = lm_server_fastdl_init();
    if (!g_voice && !g_fastdl) {
        return;
    }
    lm_on_module(LM_MOD_OBJECT, on_object);
    lm_on_module_unload(LM_MOD_OBJECT, on_object_unload);
}
