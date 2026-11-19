/* [Voice] Enabled: voice chat with the other players on the server, through the LANCommander Interposer's voice API.
 *
 * The voice library is the Interposer, whose interposer_comms_* exports carry the voice client: the one that loaded
 * the Interposer plugin build, else the one embedded in the dinput.dll / ASI builds (the patch's own image exports
 * them). Without it the game is left as it is. The game server runs this patch too (voice_server.c), which mirrors its
 * players, teams and positions into a voice server and advertises that server's port in its GameSpy query replies.
 * This client:
 * - finds the server the game plays on (voice_game.c: the server the game connected to, else the game's own
 *   traffic) and asks its query ports for the voice port (voice_net.c);
 * - connects to the voice server as the local player (the server knows the game's players and only lets each be
 *   claimed by its own client id / name: the server's ids are client id + 1);
 * - talks to the players nearby while the proximity key is held (or always while the microphone hears speech, with
 *   [Voice] VoiceActivation=1), and to the own team while the radio key is held (team games only: the server shows
 *   each player their own team's group, which becomes the radio's target);
 * - shows who is talking on the HUD (voice_game.c).
 * Everything runs on the game thread, once a frame. */
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"
#include "interposer/LANCommander.Interposer.Comms.Client.h"

#define UPDATE_MS       100
#define RECONNECT_MS    5000
#define LEAVE_MS        5000         /* no local player this long: left the server */
#define SLOT_NEARBY     0
#define SLOT_RADIO      1

/* the Interposer's voice exports: p_comms_<name> is interposer_comms_<name> */
#define COMMS_FUNCTIONS(X) \
    X(uint32_t, comms_get_api_version, (void)) \
    X(const char *, comms_get_version_string, (void)) \
    X(const char *, comms_result_to_string, (interposer_comms_result)) \
    X(void, comms_set_log_handler, (interposer_comms_log_fn, interposer_comms_log_level, void *)) \
    X(void, comms_client_config_init, (interposer_comms_client_config *)) \
    X(interposer_comms_result, comms_client_create, \
      (const interposer_comms_client_config *, interposer_comms_client **)) \
    X(void, comms_connect_info_init, (interposer_comms_connect_info *)) \
    X(interposer_comms_result, comms_client_connect, \
      (interposer_comms_client *, const interposer_comms_connect_info *)) \
    X(interposer_comms_result, comms_client_disconnect, (interposer_comms_client *)) \
    X(interposer_comms_connection_state, comms_client_get_state, (interposer_comms_client *)) \
    X(interposer_comms_result, comms_client_poll_event, (interposer_comms_client *, interposer_comms_client_event *)) \
    X(interposer_comms_result, comms_client_set_talk_targets, \
      (interposer_comms_client *, const interposer_comms_talk_target *, uint32_t)) \
    X(interposer_comms_result, comms_client_set_ptt_targets, \
      (interposer_comms_client *, uint32_t, const interposer_comms_talk_target *, uint32_t)) \
    X(interposer_comms_result, comms_client_set_push_to_talk, (interposer_comms_client *, uint32_t, int)) \
    X(interposer_comms_result, comms_client_get_group_info, \
      (interposer_comms_client *, interposer_comms_group_id, interposer_comms_client_group_info *)) \
    X(interposer_comms_result, comms_client_get_player_info, \
      (interposer_comms_client *, interposer_comms_player_id, interposer_comms_client_player_info *)) \
    X(interposer_comms_result, comms_client_get_active_speakers, \
      (interposer_comms_client *, interposer_comms_speaker_info *, uint32_t, uint32_t *)) \
    X(interposer_comms_result, comms_client_get_transmit_state, \
      (interposer_comms_client *, interposer_comms_transmit_state *)) \
    X(interposer_comms_result, comms_client_set_input_gain, (interposer_comms_client *, float)) \
    X(interposer_comms_result, comms_client_set_master_volume, (interposer_comms_client *, float)) \
    X(interposer_comms_result, comms_client_set_transmit_mode, \
      (interposer_comms_client *, interposer_comms_transmit_mode)) \
    X(interposer_comms_result, comms_list_audio_devices, \
      (interposer_comms_device_type, interposer_comms_audio_device *, uint32_t, uint32_t *)) \
    X(interposer_comms_result, comms_client_set_capture_device, (interposer_comms_client *, const char *))

#define DECLARE(ret, name, args) static ret (INTERPOSER_CALL *p_##name) args;
COMMS_FUNCTIONS(DECLARE)

static HMODULE g_interposer;          /* the Interposer: the one that loaded the plugin build, else the patch itself */
static HMODULE g_dll;

void lm_voice_set_interposer(HMODULE interposer)
{
    g_interposer = interposer;
}

HMODULE lm_interposer(void)
{
    return g_interposer;
}

/* the Interposer with the voice API this patch was built for; NULL without one. The game server
 * (voice_server_win.c) uses it too. */
HMODULE lm_voice_library(void)
{
    static int tried;
    char path[MAX_PATH];
    uint32_t (INTERPOSER_CALL *version)(void);
    if (tried) {
        return g_dll;
    }
    tried = 1;
    if (!g_interposer) {
        lm_log("voice: no LANCommander Interposer, no voice chat");
        return NULL;
    }
    GetModuleFileNameA(g_interposer, path, MAX_PATH);
    version = (uint32_t (INTERPOSER_CALL *)(void))(void (*)(void))GetProcAddress(g_interposer,
                                                                                 "interposer_comms_get_api_version");
    if (!version || version() != INTERPOSER_COMMS_API_VERSION) {
        lm_log("voice: %s has voice API version %u, the patch needs %u; no voice chat", path, version ? version() : 0,
               INTERPOSER_COMMS_API_VERSION);
        return NULL;
    }
    return g_dll = g_interposer;
}

static int resolve(void)
{
#define RESOLVE(ret, name, args) \
    if (!(p_##name = (ret (INTERPOSER_CALL *) args)(void (*)(void))GetProcAddress(g_dll, "interposer_" #name))) { \
        lm_log("voice: interposer_%s is missing from the Interposer", #name); \
        return 0; \
    }
    COMMS_FUNCTIONS(RESOLVE)
    lm_log("voice: LANCommander Interposer voice %s", p_comms_get_version_string());
    return 1;
}

/* any of the library's threads (lm_log is) */
static void INTERPOSER_CALL voice_log(interposer_comms_log_level level, const char *message, void *user)
{
    static const char *const names[] = {"trace", "debug", "info", "warn", "error"};
    (void)user;
    lm_log("voice: [interposer %s] %s", level <= INTERPOSER_COMMS_LOG_ERROR ? names[level] : "?", message);
}

/* ---------------------------------------------------------------- keys (voice_keys.c: game controls) */

static int game_has_focus(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

/* ---------------------------------------------------------------- the client */

static interposer_comms_client *g_client;
static int g_failed, g_voice_activation;
static WORD g_port = INTERPOSER_COMMS_DEFAULT_PORT;     /* if the game server does not say */
static char g_password[128], g_fixed_host[256];
static interposer_comms_group_id g_team;
static int g_held[2], g_applied_va = -1, g_applied_volume = -1, g_applied_mic_volume = -1;
static char g_applied_mic[256];
static int g_claim_by_name;          /* the server did not know our id: claim by name */
static ULONGLONG g_last_update, g_last_attempt, g_seen;
static lm_voice_state g_state;

const lm_voice_state *lm_voice_now(void)
{
    return &g_state;
}

static int create(void)
{
    interposer_comms_client_config config;
    interposer_comms_talk_target nearby = {INTERPOSER_COMMS_TARGET_PROXIMITY, 0};
    char mic[256];
    p_comms_set_log_handler(voice_log, INTERPOSER_COMMS_LOG_INFO, NULL);
    p_comms_client_config_init(&config);
    config.transmit_mode = INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK;   /* update_activation sets it */
    config.voice_activation.auto_threshold = 1;
    lm_ini_str("Voice", "Microphone", mic, sizeof mic);
    config.capture_device_id = mic[0] ? mic : NULL;
    if (p_comms_client_create(&config, &g_client) != INTERPOSER_COMMS_OK
        && (!mic[0] || (config.capture_device_id = NULL,
                        p_comms_client_create(&config, &g_client) != INTERPOSER_COMMS_OK))) {
        lm_log("voice: the voice client could not be created");
        return 0;
    }
    lstrcpynA(g_applied_mic, mic, sizeof g_applied_mic);
    p_comms_client_set_talk_targets(g_client, &nearby, 1);
    p_comms_client_set_ptt_targets(g_client, SLOT_NEARBY, &nearby, 1);
    lm_log("voice: ready (%s)", lm_ini_int("Voice", "VoiceActivation", 0) ? "voice activation" : "push to talk");
    return 1;
}

/* the settings that can change while the game runs (the options menu, the console): volumes, microphone, mode */
static void update_settings(void)
{
    int volume = lm_ini_int("Voice", "Volume", 100), mic_volume = lm_ini_int("Voice", "MicVolume", 100);
    char mic[256];
    if (volume != g_applied_volume) {
        g_applied_volume = volume;
        p_comms_client_set_master_volume(g_client, (volume < 0 ? 0 : volume > 200 ? 200 : volume) / 100.0f);
    }
    if (mic_volume != g_applied_mic_volume) {
        g_applied_mic_volume = mic_volume;
        p_comms_client_set_input_gain(g_client, (mic_volume < 0 ? 0 : mic_volume > 200 ? 200 : mic_volume) / 100.0f);
    }
    lm_ini_str("Voice", "Microphone", mic, sizeof mic);
    if (strcmp(mic, g_applied_mic)) {
        interposer_comms_result r = p_comms_client_set_capture_device(g_client, mic[0] ? mic : NULL);
        lm_log("voice: microphone %s: %s", mic[0] ? mic : "(the default)", p_comms_result_to_string(r));
        lstrcpynA(g_applied_mic, mic, sizeof g_applied_mic);
    }
    g_voice_activation = lm_ini_int("Voice", "VoiceActivation", 0) != 0;
}

/* the microphones, for the options menu (the first n of them); the number there is */
int lm_voice_microphones(lm_voice_device *out, int n)
{
    static interposer_comms_audio_device list[32];
    uint32_t count = 0, i;
    if (!g_dll || (!p_comms_list_audio_devices && !resolve())
        || p_comms_list_audio_devices(INTERPOSER_COMMS_DEVICE_CAPTURE, list, LM_COUNT(list), &count)
               != INTERPOSER_COMMS_OK) {
        return 0;
    }
    for (i = 0; i < count && (int)i < n; i++) {
        lstrcpynA(out[i].id, list[i].id, sizeof out[i].id);
        lstrcpynA(out[i].name, list[i].name, sizeof out[i].name);
    }
    return (int)(count < LM_COUNT(list) ? count : LM_COUNT(list));
}

static void connect_to(const char *host, WORD port, const char *name, int id)
{
    interposer_comms_connect_info info;
    interposer_comms_result r;
    p_comms_connect_info_init(&info);
    info.host = host;
    info.port = port;
    info.game_player_id = g_claim_by_name ? 0 : (uint64_t)id + 1;   /* the server's ids are client id + 1 */
    info.player_name = name;
    info.password = g_password[0] ? g_password : NULL;
    r = p_comms_client_connect(g_client, &info);
    lm_log("voice: connecting to %s:%u as '%s' (id %d): %s", host, port, name, id, p_comms_result_to_string(r));
}

static void update_connection(ULONGLONG now)
{
    char name[64], host[256];
    int id, team, in_game = lm_game_local_player(name, sizeof name, &id, &team);
    interposer_comms_connection_state state = p_comms_client_get_state(g_client);
    WORD game_port = 0, voice_port, ports[8];
    DWORD addr;
    int r, n;
    if (in_game) {
        g_seen = now;
    } else {
        lm_voice_query_cancel();
        if (state != INTERPOSER_COMMS_STATE_DISCONNECTED && now - g_seen > LEAVE_MS) {
            lm_log("voice: left the game, disconnecting");
            p_comms_client_disconnect(g_client);
        }
        return;
    }
    /* waiting for the game server to say which port its voice server is on */
    if (lm_voice_query_active()) {
        if (!(r = lm_voice_query_poll(&voice_port))) {
            return;
        }
        if (!lm_voice_query_address()[0]) {
            lm_log("voice: cannot resolve the game server's name; trying again");
            return;
        }
        if (r < 0) {
            lm_log("voice: the game server at %s does not say where its voice server is; trying port %u",
                   lm_voice_query_address(), g_port);
        }
        connect_to(lm_voice_query_address(), r > 0 ? voice_port : g_port, name, id);
        return;
    }
    if (state != INTERPOSER_COMMS_STATE_DISCONNECTED || now - g_last_attempt < RECONNECT_MS) {
        return;
    }
    /* [Voice] Host, else the server the game connected to, else the game's traffic */
    g_last_attempt = now;
    if (g_fixed_host[0]) {
        connect_to(g_fixed_host, g_port, name, id);
        return;
    }
    if (!lm_game_server(host, sizeof host, &game_port)) {
        if (!lm_voice_traffic_server(&addr, &game_port)) {
            g_last_attempt = 0;
            return;
        }
        snprintf(host, sizeof host, "%lu.%lu.%lu.%lu", addr >> 24, (addr >> 16) & 0xff, (addr >> 8) & 0xff,
                 addr & 0xff);
    }
    n = lm_voice_query_ports(game_port, ports, 8);
    if (!lm_voice_query_start(host, ports, n, game_port)) {
        connect_to(host, g_port, name, id);  /* no query ports (or no network) */
    }
}

static void update_keys(void)
{
    int focus = game_has_focus(), i, held[2];
    held[0] = focus && lm_voice_key_held(0);            /* the game controls of voice_keys.c */
    held[1] = focus && g_team && lm_voice_key_held(1);
    for (i = 0; i < 2; i++) {
        if (held[i] != g_held[i]) {
            g_held[i] = held[i];
            p_comms_client_set_push_to_talk(g_client, i == 0 ? SLOT_NEARBY : SLOT_RADIO, held[i]);
        }
    }
}

/* voice activation sends to the players nearby whenever the microphone hears speech; the keys still send to their
 * own targets */
static void update_activation(void)
{
    interposer_comms_talk_target nearby = {INTERPOSER_COMMS_TARGET_PROXIMITY, 0};
    if (g_voice_activation == g_applied_va) {
        return;
    }
    if (g_voice_activation) {
        p_comms_client_set_talk_targets(g_client, &nearby, 1);
    }
    p_comms_client_set_transmit_mode(g_client, g_voice_activation ? INTERPOSER_COMMS_TRANSMIT_VOICE_ACTIVITY
                                                                  : INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK);
    g_applied_va = g_voice_activation;
}

static void drain_events(void)
{
    interposer_comms_client_event e;
    while (p_comms_client_poll_event(g_client, &e) == INTERPOSER_COMMS_OK) {
        switch (e.type) {
        case INTERPOSER_COMMS_CLIENT_EVENT_STATE_CHANGED:
            lm_log("voice: %s%s%s", e.state == INTERPOSER_COMMS_STATE_CONNECTED ? "connected"
                                    : e.state == INTERPOSER_COMMS_STATE_DISCONNECTED ? "disconnected" : "connecting",
                   e.reason ? ": " : "", e.reason ? p_comms_result_to_string((interposer_comms_result)e.reason) : "");
            if (e.state == INTERPOSER_COMMS_STATE_DISCONNECTED) {
                g_team = INTERPOSER_COMMS_INVALID_ID;
                if (e.reason == INTERPOSER_COMMS_ERR_AUTH_FAILED) {
                    g_claim_by_name = !g_claim_by_name;   /* no player with our id: try by name, and back */
                    g_last_attempt = 0;
                }
            }
            break;
        case INTERPOSER_COMMS_CLIENT_EVENT_GROUP_ADDED: {
            /* the server shows us our own team's group only: the radio talks to it */
            interposer_comms_client_group_info info;
            if (p_comms_client_get_group_info(g_client, e.group, &info) == INTERPOSER_COMMS_OK
                && info.type == INTERPOSER_COMMS_GROUP_SERVER && info.is_member) {
                interposer_comms_talk_target radio;
                radio.kind = INTERPOSER_COMMS_TARGET_GROUP;
                radio.id = e.group;
                g_team = e.group;
                p_comms_client_set_ptt_targets(g_client, SLOT_RADIO, &radio, 1);
                lm_log("voice: team radio %s", info.name);
            }
            break;
        }
        case INTERPOSER_COMMS_CLIENT_EVENT_GROUP_REMOVED:
            if (e.group == g_team) {
                g_team = INTERPOSER_COMMS_INVALID_ID;
            }
            break;
        case INTERPOSER_COMMS_CLIENT_EVENT_AUDIO_DEVICE_LOST:
            lm_log("voice: audio device lost: %s", e.text);
            break;
        case INTERPOSER_COMMS_CLIENT_EVENT_KICKED:
            lm_log("voice: removed from voice: %s", e.text);
            break;
        default:
            break;
        }
    }
}

/* who is talking, for the HUD */
static void update_state(void)
{
    interposer_comms_speaker_info speakers[LM_VOICE_MAX_TALKERS];
    interposer_comms_transmit_state tx;
    static char text[LM_VOICE_MAX_TALKERS + 1][96];
    const char *lines[LM_VOICE_MAX_TALKERS + 1];
    uint32_t n = 0, i;
    int k = 0;
    lm_voice_state *s = &g_state;
    s->connected = p_comms_client_get_state(g_client) == INTERPOSER_COMMS_STATE_CONNECTED;
    s->n = 0;
    s->talking = s->radio = 0;
    if (s->connected && p_comms_client_get_transmit_state(g_client, &tx) == INTERPOSER_COMMS_OK) {
        s->talking = tx.transmitting;
        s->radio = (tx.ptt_slots_held & (1u << SLOT_RADIO)) != 0;
    }
    if (s->connected
        && p_comms_client_get_active_speakers(g_client, speakers, LM_VOICE_MAX_TALKERS, &n) == INTERPOSER_COMMS_OK) {
        for (i = 0; i < n && i < LM_VOICE_MAX_TALKERS; i++) {
            interposer_comms_client_player_info info;
            lm_voice_talker *t = &s->talkers[s->n];
            if (speakers[i].locally_muted
                || p_comms_client_get_player_info(g_client, speakers[i].player, &info) != INTERPOSER_COMMS_OK) {
                continue;
            }
            lstrcpynA(t->name, info.name, sizeof t->name);
            t->radio = speakers[i].kind == INTERPOSER_COMMS_TARGET_GROUP;
            s->n++;
        }
    }
    if (!lm_ini_int("Voice", "ShowTalkers", 1)) {
        return;
    }
    if (s->talking) {
        snprintf(text[k], sizeof text[k], s->radio ? "* Talking (team)" : "* Talking");
        lines[k] = text[k];
        k++;
    }
    for (i = 0; i < (uint32_t)s->n; i++) {
        snprintf(text[k], sizeof text[k], "%s%s", s->talkers[i].name, s->talkers[i].radio ? " (team)" : "");
        lines[k] = text[k];
        k++;
    }
    lm_game_hud_lines(lines, k);
}

static void tick(void)
{
    ULONGLONG now;
    if (g_failed) {
        return;
    }
    if (!g_client && !(resolve() && create())) {   /* the first frame */
        g_failed = 1;
        return;
    }
    update_keys();                   /* every frame, so the keys feel immediate */
    now = GetTickCount64();
    if (now - g_last_update >= UPDATE_MS) {
        g_last_update = now;
        update_settings();
        update_connection(now);
        update_activation();
    }
    drain_events();
    update_state();
}

/* ---------------------------------------------------------------- the game's traffic */

static int (WINAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);

static int WINAPI hk_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    if (to && tolen >= (int)sizeof(struct sockaddr_in) && to->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)to;
        if (ntohs(in->sin_port) != g_port) {
            lm_voice_traffic(ntohl(in->sin_addr.s_addr), ntohs(in->sin_port));
        }
    }
    return real_sendto(s, buf, len, flags, to, tolen);
}

void lm_voice_install(void)
{
    if (!lm_ini_int("Voice", "Enabled", 1)) {
        lm_log("voice: [Voice] Enabled=0, no voice chat");
        return;
    }
    if (!lm_voice_library()) {
        return;
    }
    g_dll = lm_voice_library();
    g_port = (WORD)lm_ini_int("Voice", "Port", INTERPOSER_COMMS_DEFAULT_PORT);
    lm_ini_str("Voice", "Password", g_password, sizeof g_password);
    lm_ini_str("Voice", "Host", g_fixed_host, sizeof g_fixed_host);
    g_voice_activation = lm_ini_int("Voice", "VoiceActivation", 0) != 0;
    lm_game_install();
    lm_voice_menu_install();
    lm_voice_keys_install();
    lm_game_on_frame(tick);
    /* the game's packets go out through lithtech.exe's WSOCK32!sendto (ordinal 20) */
    real_sendto = lm_hook_import_ordinal(GetModuleHandleA(NULL), "WSOCK32.dll", 20, hk_sendto);
    if (!real_sendto) {
        real_sendto = lm_hook_import("WSOCK32.dll", "sendto", hk_sendto);
    }
}
