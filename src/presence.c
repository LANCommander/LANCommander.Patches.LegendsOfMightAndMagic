/* [RichPresence]: what the player is doing, shown in Discord and in the LANCommander launcher, through the LANCommander
 * Interposer's rich presence (InterposerSetPresence* / InterposerUpdatePresence; the Interposer built into the
 * dinput.dll / ASI builds, or the one that loads the plugin build). The Interposer talks to Discord itself when its
 * Config.yml has RichPresence.Discord (an application id: [RichPresence] DiscordApplicationId for the built-in one, see
 * interposer_config.c); the launcher hears every update either way.
 *
 * - In the menus: "In the menus".
 * - In a game (joined or hosted): the world and the game type on the first line, the server's name on the second with
 *   the number of players, and the time since the world started. They come from the game server's own GameSpy
 *   \status\ reply (hostname, mapname, gametype, numplayers, maxplayers), the reply the server browser reads: it
 *   follows map changes, and is the same for a joined server and for a game this one hosts (127.0.0.1 and the query
 *   port object.lto bound, object.lto+0x1715f4 -> +0xe4).
 *
 * The game thread only takes a snapshot of the connection, every half second (cshell's Update, voice_game.c). A thread
 * of this module asks the server and talks to the Interposer: both block (the query up to 1.5 s, Discord's pipe). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "voice.h"
#include "interposer/LANCommander.Interposer.h"

#define LTO_QUERY         0x1715f4u  /* the GameSpy qr object; +0xe4 the query port it bound */
#define QR_PORT           0xe4
#define SNAPSHOT_MS       500
#define QUERY_MS          15000      /* how often the server is asked while in a game */
#define DEFAULT_QUERY     29801

typedef void (INTERPOSER_CALL *set_str_fn)(const char *);
typedef void (INTERPOSER_CALL *set_image_fn)(const char *key, const char *text);
typedef void (INTERPOSER_CALL *set_times_fn)(long long start, long long end);
typedef void (INTERPOSER_CALL *set_party_fn)(const char *id, int size, int max);
typedef void (INTERPOSER_CALL *set_int_fn)(int);
typedef void (INTERPOSER_CALL *void_fn)(void);

static struct {
    set_str_fn details, state, name;
    set_image_fn large_image;
    set_times_fn timestamps;
    set_party_fn party;
    set_int_fn type;
    void_fn update;
} g_rp;

/* the connection, as the game thread last saw it */
typedef struct {
    int in_game;                     /* connected to a server (or hosting one) */
    int hosting;
    char host[64];                   /* the server to ask */
    WORD game_port;
    WORD ports[8];
    int n_ports;
} snapshot;

static CRITICAL_SECTION g_cs;
static snapshot g_now;
static HANDLE g_wake;
static int g_started;
static char g_large_image[MAX_PATH];

/* ---------------------------------------------------------------- the game thread */

static WORD hosted_query_port(void)
{
    HMODULE lto = lm_game_module(LM_MOD_OBJECT);
    BYTE *qr;
    int port;
    if (!lto || !lm_readable((void *)LM_RVA(lto, LTO_QUERY), 4)) {
        return 0;
    }
    qr = *(BYTE **)LM_RVA(lto, LTO_QUERY);
    if (!lm_readable(qr + QR_PORT, 4) || (port = *(int *)(qr + QR_PORT)) <= 0 || port > 65535) {
        return 0;
    }
    return (WORD)port;
}

static void take_snapshot(snapshot *s)
{
    char name[32];
    int id, team, mode = lm_game_mode();
    memset(s, 0, sizeof *s);
    if (mode == LM_GAME_MODE_NONE || !lm_game_local_player(name, sizeof name, &id, &team)) {
        return;
    }
    s->in_game = 1;
    s->hosting = mode == LM_GAME_MODE_HOST;
    if (s->hosting) {
        WORD port = hosted_query_port();
        strcpy(s->host, "127.0.0.1");
        if (port) {
            s->ports[s->n_ports++] = port;
        } else {                     /* the qr SDK binds the first free port from 29801 */
            for (port = DEFAULT_QUERY; s->n_ports < 4; port++) {
                s->ports[s->n_ports++] = port;
            }
        }
        return;
    }
    if (!lm_game_server(s->host, sizeof s->host, &s->game_port)) {
        return;                      /* connected some way the join hook did not see: no server to ask */
    }
    s->n_ports = lm_voice_query_ports(s->game_port, s->ports, LM_COUNT(s->ports));
    if (lm_game_query_port() && s->n_ports > 0) {
        s->ports[0] = lm_game_query_port();   /* the browser knows it: ask that one first */
    }
}

static DWORD WINAPI presence_thread(void *arg);

static void tick(void)
{
    static ULONGLONG next;
    ULONGLONG now = GetTickCount64();
    snapshot s;
    if (now < next) {
        return;
    }
    next = now + SNAPSHOT_MS;
    if (!g_started) {
        HANDLE t;
        g_started = 1;
        if ((t = CreateThread(NULL, 0, presence_thread, NULL, 0, NULL)) != NULL) {
            CloseHandle(t);
        } else {
            lm_log("presence: cannot start its thread (%lu), no rich presence", GetLastError());
        }
    }
    take_snapshot(&s);
    EnterCriticalSection(&g_cs);
    if (memcmp(&s, &g_now, sizeof s) != 0) {
        g_now = s;
        SetEvent(g_wake);
    }
    LeaveCriticalSection(&g_cs);
}

/* ---------------------------------------------------------------- the presence thread */

/* what is shown */
typedef struct {
    char details[128], state[128], party_id[32];
    int party_size, party_max;
    long long start;
} presence;

/* text in the ANSI code page (server names) as UTF-8 */
static void to_utf8(const char *ansi, char *out, int size)
{
    wchar_t wide[256];
    if (!MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, LM_COUNT(wide))
        || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, size, NULL, NULL)) {
        lstrcpynA(out, ansi, size);
    }
}

/* "Worlds\Castle.dat" -> "Castle" */
static void world_name(const char *map, char *out, int size)
{
    const char *base = map, *p;
    char *dot;
    for (p = map; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    lstrcpynA(out, base, size);
    if ((dot = strrchr(out, '.')) != NULL && dot != out) {
        *dot = '\0';
    }
}

static void query_value(const char *reply, const char *key, char *value, int size)
{
    if (!lm_query_value(reply, (int)strlen(reply), key, value, size)) {
        value[0] = '\0';
    }
}

/* the presence of a game from the server's \status\ reply ("" if it did not answer) */
static void game_presence(const snapshot *s, const char *reply, presence *p)
{
    char map[64] = "", type[64] = "", host_name[96] = "", text[96], players[16], max[16];
    unsigned long hash = 2166136261u;
    const char *c;
    if (reply[0]) {
        query_value(reply, "mapname", text, sizeof text);
        world_name(text, map, sizeof map);
        query_value(reply, "gametype", type, sizeof type);
        query_value(reply, "hostname", text, sizeof text);
        to_utf8(text, host_name, sizeof host_name);
        query_value(reply, "numplayers", players, sizeof players);
        query_value(reply, "maxplayers", max, sizeof max);
        p->party_size = atoi(players);
        p->party_max = atoi(max);
    }
    if (map[0] && type[0]) {
        snprintf(p->details, sizeof p->details, "%s on %s", type, map);
    } else if (map[0]) {
        snprintf(p->details, sizeof p->details, "Playing on %s", map);
    } else {
        strcpy(p->details, "In a game");
    }
    if (s->hosting) {
        snprintf(p->state, sizeof p->state, "Hosting %s", host_name[0] ? host_name : "a game");
    } else {
        snprintf(p->state, sizeof p->state, "%s", host_name[0] ? host_name : "Online");
    }
    if (p->party_size <= 0 || p->party_max < p->party_size) {
        p->party_size = p->party_max = 0;
    }
    /* the same id for everyone on the server (Discord groups them), without the address itself */
    for (c = s->host; *c; c++) {
        hash = (hash ^ (unsigned char)*c) * 16777619u;
    }
    hash = (hash ^ s->game_port) * 16777619u;
    snprintf(p->party_id, sizeof p->party_id, "lomm-%08lx", hash);
}

static void show(const presence *p)
{
    g_rp.type(0);
    g_rp.name("Legends of Might and Magic");
    g_rp.details(p->details);
    g_rp.state(p->state[0] ? p->state : NULL);
    g_rp.large_image(g_large_image[0] ? g_large_image : NULL, "Legends of Might and Magic");
    g_rp.timestamps(p->start, 0);
    g_rp.party(p->party_size ? p->party_id : NULL, p->party_size, p->party_max);
    g_rp.update();
    lm_log("presence: %s / %s%s", p->details, p->state[0] ? p->state : "-",
           p->party_size ? "" : " (no player count)");
}

static DWORD WINAPI presence_thread(void *arg)
{
    snapshot cur, last;
    presence shown, want;
    char reply[2048], last_map[64] = "", map[64];
    ULONGLONG next_query = 0;
    long long menu_start = (long long)time(NULL), game_start = 0;
    int first = 1;
    memset(&last, 0, sizeof last);
    memset(&shown, 0, sizeof shown);
    reply[0] = '\0';
    for (;;) {
        ULONGLONG now;
        WaitForSingleObject(g_wake, 1000);
        EnterCriticalSection(&g_cs);
        cur = g_now;
        LeaveCriticalSection(&g_cs);
        now = GetTickCount64();
        memset(&want, 0, sizeof want);
        if (!cur.in_game) {
            if (last.in_game) {
                menu_start = (long long)time(NULL);
            }
            strcpy(want.details, "In the menus");
            want.start = menu_start;
            reply[0] = '\0';
            last_map[0] = '\0';
        } else {
            if (!last.in_game || strcmp(cur.host, last.host) || cur.game_port != last.game_port
                || cur.hosting != last.hosting) {
                reply[0] = '\0';     /* another server: ask it at once */
                next_query = 0;
                game_start = (long long)time(NULL);
                last_map[0] = '\0';
            }
            if (now >= next_query && cur.n_ports > 0) {
                if (lm_query(cur.host, cur.ports, cur.n_ports, cur.hosting ? 0 : cur.game_port, "\\status\\", reply,
                             sizeof reply) != 1) {
                    reply[0] = '\0';
                }
                next_query = GetTickCount64() + QUERY_MS;
                query_value(reply, "mapname", map, sizeof map);
                if (map[0] && strcmp(map, last_map)) {
                    if (last_map[0]) {
                        game_start = (long long)time(NULL);   /* a new world on the same server */
                    }
                    lstrcpynA(last_map, map, sizeof last_map);
                }
            }
            game_presence(&cur, reply, &want);
            want.start = game_start;
        }
        last = cur;
        if (first || memcmp(&want, &shown, sizeof want) != 0) {
            first = 0;
            shown = want;
            show(&shown);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- install */

void lm_presence_install(void)
{
    HMODULE ip = lm_interposer();
    if (!lm_ini_int("RichPresence", "Enabled", 1)) {
        lm_log("presence: [RichPresence] Enabled=0, no rich presence");
        return;
    }
    if (!ip) {
        lm_log("presence: no LANCommander Interposer, no rich presence");
        return;
    }
#define RESOLVE(field, type, name) g_rp.field = (type)(void (*)(void))GetProcAddress(ip, name)
    RESOLVE(details, set_str_fn, "InterposerSetPresenceDetails");
    RESOLVE(state, set_str_fn, "InterposerSetPresenceState");
    RESOLVE(name, set_str_fn, "InterposerSetPresenceName");
    RESOLVE(large_image, set_image_fn, "InterposerSetPresenceLargeImage");
    RESOLVE(timestamps, set_times_fn, "InterposerSetPresenceTimestamps");
    RESOLVE(party, set_party_fn, "InterposerSetPresenceParty");
    RESOLVE(type, set_int_fn, "InterposerSetPresenceType");
    RESOLVE(update, void_fn, "InterposerUpdatePresence");
#undef RESOLVE
    if (!g_rp.details || !g_rp.state || !g_rp.name || !g_rp.large_image || !g_rp.timestamps || !g_rp.party
        || !g_rp.type || !g_rp.update) {
        lm_log("presence: the LANCommander Interposer in the game has no rich presence API; update it");
        return;
    }
    lm_ini_str("RichPresence", "LargeImage", g_large_image, sizeof g_large_image);
    InitializeCriticalSection(&g_cs);
    if (!(g_wake = CreateEventA(NULL, FALSE, FALSE, NULL))) {
        lm_log("presence: CreateEvent failed (%lu), no rich presence", GetLastError());
        return;
    }
    lm_game_install();
    lm_game_on_frame(tick);
    lm_log("presence: on (Discord through the Interposer's RichPresence.Discord, and the LANCommander launcher)");
}
