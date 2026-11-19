/* Voice on the game server (a listen server in lithtech.exe, or the dedicated server LegendServ.exe; the game side
 * is voice_server_win.c): hosts the voice server of the LANCommander Interposer's voice API and mirrors the game into
 * it, so the game clients (voice.c) find it and talk to whom the game says.
 * - Every player is registered under game id client id + 1 (the voice API keeps 0 for none); clients log in by
 *   claiming their game player (INTERPOSER_COMMS_AUTH_CLAIM), so voice always matches who is on the game server.
 * - In team games the teams are server groups: each player sees their own team's only, its team radio.
 * - The live players' positions drive proximity chat 20 times a second; the dead drop out of proximity but keep the
 *   radio.
 * - The voice port is advertised as \commsport\ in the game's GameSpy query replies (voice_server_win.c's sendto hook
 *   calls lm_server_query_reply), so clients do not need it set.
 * - So is the FastDL server the worlds are downloaded from ([Server] FastDLPort, FastDLHost, FastDLPath), as
 *   \fastdlport\, \fastdlhost\ and \fastdlpath\: the clients hand it to their Interposer (fastdl.c). That needs no
 *   voice server.
 * Everything but lm_server_query_reply runs on the game thread, from the server's frame hook. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lommpatch.h"
#include "voice_server.h"
#include "interposer/LANCommander.Interposer.Comms.Server.h"

#define SYNC_MS         50
#define STATUS_MS       30000
#define MAX_PLAYERS     256

/* the Interposer's voice server exports: p_<name> is interposer_comms_<name> */
#define SERVER_FUNCTIONS(X) \
    X(uint32_t, get_api_version, (void)) \
    X(const char *, result_to_string, (interposer_comms_result)) \
    X(void, set_log_handler, (interposer_comms_log_fn, interposer_comms_log_level, void *)) \
    X(void, server_config_init, (interposer_comms_server_config *)) \
    X(interposer_comms_result, server_create, (const interposer_comms_server_config *, interposer_comms_server **)) \
    X(interposer_comms_result, server_start, (interposer_comms_server *)) \
    X(interposer_comms_result, server_stop, (interposer_comms_server *)) \
    X(void, server_destroy, (interposer_comms_server *)) \
    X(interposer_comms_result, server_poll_event, (interposer_comms_server *, interposer_comms_server_event *)) \
    X(interposer_comms_result, server_add_player, \
      (interposer_comms_server *, uint64_t, const char *, interposer_comms_player_id *)) \
    X(interposer_comms_result, server_remove_player, (interposer_comms_server *, interposer_comms_player_id)) \
    X(void, zone_config_init, (interposer_comms_zone_config *)) \
    X(interposer_comms_result, server_configure_zone, \
      (interposer_comms_server *, interposer_comms_zone_id, const interposer_comms_zone_config *)) \
    X(interposer_comms_result, server_update_player_position, \
      (interposer_comms_server *, interposer_comms_player_id, interposer_comms_zone_id, interposer_comms_vec3, \
       const interposer_comms_vec3 *)) \
    X(interposer_comms_result, server_clear_player_position, (interposer_comms_server *, interposer_comms_player_id)) \
    X(void, group_config_init, (interposer_comms_group_config *)) \
    X(interposer_comms_result, server_create_group, \
      (interposer_comms_server *, const interposer_comms_group_config *, interposer_comms_group_id *)) \
    X(interposer_comms_result, server_add_to_group, \
      (interposer_comms_server *, interposer_comms_group_id, interposer_comms_player_id)) \
    X(interposer_comms_result, server_remove_from_group, \
      (interposer_comms_server *, interposer_comms_group_id, interposer_comms_player_id))

#define DECLARE(ret, name, args) static ret (INTERPOSER_CALL *p_##name) args;
SERVER_FUNCTIONS(DECLARE)

static struct {
    int tracked;
    interposer_comms_player_id voice;
    char name[64];
    int team;
    int positioned;
} g_players[MAX_PLAYERS];

static interposer_comms_server *g_server;
static interposer_comms_group_id g_teams[LM_SERVER_TEAMS + 1];
static int g_failed;
static uint64_t g_last_sync, g_last_status;
static volatile uint16_t g_port;     /* advertised once the voice server runs */
static lm_server_player g_read[MAX_PLAYERS];

static int resolve(void)
{
#define RESOLVE(ret, name, args) \
    if (!(p_##name = (ret (INTERPOSER_CALL *) args)lm_server_symbol("interposer_comms_" #name))) { \
        lm_log("voice server: the Interposer has no interposer_comms_%s, no voice", #name); \
        return 0; \
    }
    SERVER_FUNCTIONS(RESOLVE)
    if (p_get_api_version() != INTERPOSER_COMMS_API_VERSION) {
        lm_log("voice server: the Interposer has voice API version %u, the patch needs %u; no voice",
               p_get_api_version(), INTERPOSER_COMMS_API_VERSION);
        return 0;
    }
    return 1;
}

/* any of the library's threads (lm_log is) */
static void INTERPOSER_CALL voice_log(interposer_comms_log_level level, const char *message, void *user)
{
    static const char *const names[] = {"trace", "debug", "info", "warn", "error"};
    (void)user;
    lm_log("voice server: [interposer %s] %s", level <= INTERPOSER_COMMS_LOG_ERROR ? names[level] : "?", message);
}

static float setting_float(const char *key, float def)
{
    char text[32];
    return lm_ini_str("Server", key, text, sizeof text) ? (float)atof(text) : def;
}

static int start(void)
{
    char password[128], admin[128], bind[64];
    interposer_comms_server_config config;
    interposer_comms_zone_config zone;
    interposer_comms_group_config team;
    interposer_comms_result r;
    int t;
    if (!resolve()) {
        return 0;
    }
    p_set_log_handler(voice_log, INTERPOSER_COMMS_LOG_INFO, NULL);
    lm_ini_str("Voice", "Password", password, sizeof password);
    lm_ini_str("Server", "VoiceAdminKey", admin, sizeof admin);
    lm_ini_str("Server", "VoiceBindAddress", bind, sizeof bind);
    p_server_config_init(&config);
    config.bind_address = bind[0] ? bind : NULL;
    config.port = (uint16_t)lm_ini_int("Voice", "Port", INTERPOSER_COMMS_DEFAULT_PORT);
    config.name = "Legends of Might and Magic";
    config.auth_mode = INTERPOSER_COMMS_AUTH_CLAIM;
    config.password = password[0] ? password : NULL;
    config.admin_key = admin[0] ? admin : NULL;
    config.max_players = (uint32_t)lm_ini_int("Server", "VoiceMaxPlayers", 32);
    config.max_private_groups_per_player = 1;
    config.opus_max_bitrate = (uint32_t)lm_ini_int("Server", "VoiceMaxBitrate", 32000);
    config.position_authority = INTERPOSER_COMMS_POSITION_SERVER;
    r = p_server_create(&config, &g_server);
    if (r == INTERPOSER_COMMS_OK) {
        r = p_server_start(g_server);
    }
    if (r != INTERPOSER_COMMS_OK) {
        lm_log("voice server: cannot start on port %u: %s", config.port, p_result_to_string(r));
        if (g_server) {
            p_server_destroy(g_server);
            g_server = NULL;
        }
        return 0;
    }
    /* LithTech is Y up and left-handed, in world units: the ranges are set in metres (LithTech's tool metric, 52.49
     * units per metre) */
    p_zone_config_init(&zone);
    zone.max_distance = setting_float("VoiceRange", 50.0f) * LM_UNITS_PER_METRE;
    zone.min_distance = setting_float("VoiceFullVolumeRange", 4.0f) * LM_UNITS_PER_METRE;
    zone.up.x = 0.0f;
    zone.up.y = 1.0f;
    zone.up.z = 0.0f;
    zone.left_handed = 1;
    if (p_server_configure_zone(g_server, 0, &zone) != INTERPOSER_COMMS_OK) {
        lm_log("voice server: [Server] VoiceRange / VoiceFullVolumeRange are not valid, using the defaults");
    }
    p_group_config_init(&team);
    team.type = INTERPOSER_COMMS_GROUP_SERVER;
    for (t = 1; t <= LM_SERVER_TEAMS; t++) {
        team.name = lm_server_team_name(t);
        p_server_create_group(g_server, &team, &g_teams[t]);
    }
    g_port = config.port;
    lm_log("voice server: listening on port %u", config.port);
    return 1;
}

static void forget(int id)
{
    p_server_remove_player(g_server, g_players[id].voice);
    g_players[id].tracked = 0;
}

static void sync_players(uint64_t now)
{
    static unsigned char seen[MAX_PLAYERS];
    int n = lm_server_read_players(g_read, MAX_PLAYERS), i, tracked = 0;
    if (n < 0) {
        return;                      /* between worlds: the last known state stays */
    }
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        const lm_server_player *p = &g_read[i];
        int positioned;
        if (p->id < 0 || p->id >= MAX_PLAYERS || !p->name[0]) {
            continue;
        }
        seen[p->id] = 1;
        if (g_players[p->id].tracked && strcmp(g_players[p->id].name, p->name)) {
            forget(p->id);           /* the id went to someone else (or a rename): start over */
        }
        if (!g_players[p->id].tracked) {
            interposer_comms_result r = p_server_add_player(g_server, (uint64_t)p->id + 1, p->name,
                                                            &g_players[p->id].voice);
            if (r != INTERPOSER_COMMS_OK) {
                lm_log("voice server: cannot add player %d '%s': %s", p->id, p->name, p_result_to_string(r));
                continue;
            }
            g_players[p->id].tracked = 1;
            g_players[p->id].team = -1;
            g_players[p->id].positioned = 0;
            snprintf(g_players[p->id].name, sizeof g_players[p->id].name, "%s", p->name);
            lm_log("voice server: player %d '%s' joined (team %d)", p->id, p->name, p->team);
        }
        if (g_players[p->id].team != p->team) {
            int old = g_players[p->id].team;
            if (old >= 1 && old <= LM_SERVER_TEAMS) {
                p_server_remove_from_group(g_server, g_teams[old], g_players[p->id].voice);
            }
            if (p->team >= 1 && p->team <= LM_SERVER_TEAMS) {
                p_server_add_to_group(g_server, g_teams[p->team], g_players[p->id].voice);
            }
            g_players[p->id].team = p->team;
        }
        positioned = p->alive && p->has_position;
        if (positioned) {
            interposer_comms_vec3 at, forward;
            at.x = p->position[0];
            at.y = p->position[1];
            at.z = p->position[2];
            forward.x = p->forward[0];
            forward.y = p->forward[1];
            forward.z = p->forward[2];
            /* the facing is what the voice server pans by: without it everyone is heard dead centre */
            p_server_update_player_position(g_server, g_players[p->id].voice, 0, at, &forward);
        } else if (g_players[p->id].positioned) {
            p_server_clear_player_position(g_server, g_players[p->id].voice);
        }
        g_players[p->id].positioned = positioned;
    }
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].tracked && !seen[i]) {
            lm_log("voice server: player %d '%s' left", i, g_players[i].name);
            forget(i);
        }
        tracked += g_players[i].tracked;
    }
    if (now - g_last_status >= STATUS_MS) {
        g_last_status = now;
        lm_log("voice server: status: %d game players, %d in voice", n, tracked);
    }
}

static void drain_events(void)
{
    interposer_comms_server_event e;
    while (p_server_poll_event(g_server, &e) == INTERPOSER_COMMS_OK) {
        switch (e.type) {
        case INTERPOSER_COMMS_SERVER_EVENT_PLAYER_CONNECTED:
            lm_log("voice server: voice player %u connected", e.player);
            break;
        case INTERPOSER_COMMS_SERVER_EVENT_PLAYER_DISCONNECTED:
            lm_log("voice server: voice player %u disconnected %s", e.player, e.text);
            break;
        case INTERPOSER_COMMS_SERVER_EVENT_AUTH_FAILED:
            lm_log("voice server: a login was refused: %s", e.text);
            break;
        case INTERPOSER_COMMS_SERVER_EVENT_ADMIN_COMMAND:
            lm_log("voice server: admin command: %s", e.text);
            break;
        default:
            break;
        }
    }
}

void lm_server_tick(void)
{
    uint64_t now;
    if (g_failed) {
        return;
    }
    if (!g_server && !start()) {
        g_failed = 1;
        return;
    }
    now = lm_server_now_ms();
    if (now - g_last_sync >= SYNC_MS) {
        g_last_sync = now;
        sync_players(now);
    }
    drain_events();
}

void lm_server_stop(void)
{
    int i;
    if (g_server) {
        g_port = 0;
        p_server_stop(g_server);
        p_server_destroy(g_server);
        g_server = NULL;
        lm_log("voice server: stopped");
    }
    for (i = 0; i < MAX_PLAYERS; i++) {
        g_players[i].tracked = 0;
    }
    g_failed = 0;                    /* the next server (a new world, a new game) starts it again */
}

/* ---------------------------------------------------------------- the query replies */

/* the next '\' at or after at, else length */
static int next_slash(const char *p, int at, int length)
{
    while (at < length && p[at] != '\\') {
        at++;
    }
    return at;
}

/* \fastdlport\<port>, then \fastdlhost\<host> and \fastdlpath\<path> if they are set; "" without FastDL */
static char g_fastdl[2 * MAX_PATH + 64];

int lm_server_fastdl_init(void)
{
    char host[MAX_PATH], path[MAX_PATH];
    int port = lm_ini_int("Server", "FastDLPort", 0), n;
    g_fastdl[0] = '\0';
    lm_ini_str("Server", "FastDLHost", host, sizeof host);
    lm_ini_str("Server", "FastDLPath", path, sizeof path);
    if (port <= 0 || port > 65535) {
        if (port) {
            lm_log("fastdl: FastDLPort %d is not a port, FastDL is not advertised", port);
        }
        return 0;
    }
    /* the replies are \key\value: a backslash would end the value */
    if (strchr(host, '\\') || strchr(path, '\\')) {
        lm_log("fastdl: FastDLHost and FastDLPath cannot hold a backslash, FastDL is not advertised");
        return 0;
    }
    n = snprintf(g_fastdl, sizeof g_fastdl, "\\fastdlport\\%d", port);
    if (host[0]) {
        n += snprintf(g_fastdl + n, sizeof g_fastdl - n, "\\fastdlhost\\%s", host);
    }
    if (path[0]) {
        snprintf(g_fastdl + n, sizeof g_fastdl - n, "\\fastdlpath\\%s%s", path[0] == '/' ? "" : "/", path);
    }
    lm_log("fastdl: the query replies say the FastDL server is http://%s:%d%s%s", host[0] ? host : "<this server>",
           port, path[0] == '/' ? "" : "/", path);
    return 1;
}

int lm_server_query_reply(const char *packet, int length, char *out, int size)
{
    static const char key[] = "commsport", fastdl_key[] = "fastdlport";
    char pairs[sizeof g_fastdl + 32];
    int pos = 0, final_at = -1, key_end, value_end, n, i;
    uint16_t port = g_port;
    if ((!port && !g_fastdl[0]) || !packet || length < 7 || packet[0] != '\\') {
        return 0;
    }
    /* cheap rejection first: this sees every packet the game server sends */
    for (i = 0; i + 7 <= length && memcmp(packet + i, "\\final\\", 7); i++) {
    }
    if (i + 7 > length) {
        return 0;
    }
    /* \key\value pairs; the new ones go before the first \final\ */
    while (pos < length) {
        key_end = next_slash(packet, pos + 1, length);
        if (key_end == length) {
            return 0;                /* a key without a value: not a reply */
        }
        value_end = next_slash(packet, key_end + 1, length);
        n = key_end - pos - 1;
        if ((n == (int)sizeof key - 1 && !memcmp(packet + pos + 1, key, (size_t)n))
            || (n == (int)sizeof fastdl_key - 1 && !memcmp(packet + pos + 1, fastdl_key, (size_t)n))) {
            return 0;
        }
        if (n == 5 && !memcmp(packet + pos + 1, "final", 5) && final_at < 0) {
            final_at = pos;
        }
        pos = value_end;
    }
    n = port ? snprintf(pairs, sizeof pairs, "\\%s\\%u", key, port) : 0;
    n += snprintf(pairs + n, sizeof pairs - n, "%s", g_fastdl);
    if (final_at < 0 || length + n > size) {
        return 0;
    }
    memcpy(out, packet, (size_t)final_at);
    memcpy(out + final_at, pairs, (size_t)n);
    memcpy(out + final_at + n, packet + final_at, (size_t)(length - final_at));
    return length + n;
}
