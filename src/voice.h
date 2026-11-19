/* Voice chat in the game: what voice.c, voice_net.c, voice_hud and fastdl.c share. */
#ifndef LM_VOICE_H
#define LM_VOICE_H
#include "lommpatch.h"

/* ---------------------------------------------------------------- voice.c */

/* what the HUD shows, updated on the game thread every frame */
#define LM_VOICE_MAX_TALKERS 12
typedef struct {
    char name[64];
    int radio;                       /* on the team radio (else heard nearby) */
} lm_voice_talker;

typedef struct {
    int connected;
    int talking;                     /* the local player's voice is being sent */
    int radio;                       /* to the team */
    int n;
    lm_voice_talker talkers[LM_VOICE_MAX_TALKERS];
} lm_voice_state;

const lm_voice_state *lm_voice_now(void);
HMODULE lm_voice_library(void);      /* the Interposer with the voice API; NULL without one */
int lm_voice_key_held(int which);    /* voice_keys.c: 0 talk nearby, 1 team radio; only while the game screen is up */
void lm_voice_keys_install(void);

/* a microphone: its id (VoiceMicrophone's value) and the name Windows gives it */
typedef struct {
    char id[256];
    char name[256];
} lm_voice_device;
int lm_voice_microphones(lm_voice_device *out, int n);   /* how many there are (the first n go to out) */

/* ---------------------------------------------------------------- the game (cshell.dll, voice_game.c) */

/* the local player as the server knows them: name, client id, team (0 = none); 0 while not in a game */
int lm_game_local_player(char *name, int size, int *id, int *team);
/* the game server the client is connected to ("" while it is not): host and game port */
int lm_game_server(char *host, int size, WORD *port);
/* ILTClient GetGameMode: the StartGame type of the connection (2 = this game hosts, 3 = joined), 5 = not connected */
int lm_game_mode(void);
#define LM_GAME_MODE_HOST 2
#define LM_GAME_MODE_NONE 5
/* the query port of the server last joined, as the browser knew it (0 if it did not) */
WORD lm_game_query_port(void);
/* fn runs on the game thread once a frame */
void lm_game_on_frame(void (*fn)(void));
/* the line drawn on the HUD for the talkers (voice_hud): lines[0..n), every frame */
void lm_game_hud_lines(const char *const *lines, int n);

/* ---------------------------------------------------------------- voice_net.c */

int lm_voice_ports(const char *text, WORD *ports, int max);   /* "27888,27889" */

/* where the game sends its own packets (the game server); host byte order */
void lm_voice_traffic(DWORD addr, WORD port);
int lm_voice_traffic_server(DWORD *addr, WORD *port);

/* asks a game server for the voice port it advertises (\commsport\ in its \info\ reply) without blocking: the host
 * name is resolved on a thread of its own */
int lm_voice_query_start(const char *host, const WORD *ports, int n, WORD game_port);
int lm_voice_query_poll(WORD *voice_port);    /* 0 pending, 1 found, -1 not (the query is over) */
void lm_voice_query_cancel(void);
int lm_voice_query_active(void);
const char *lm_voice_query_address(void);     /* the host's address once resolved, else "" */

/* the same query, blocking (up to 1.5 s, and the host name's lookup): the reply (\k\v pairs) of the query port whose
 * \hostport\ is game_port (any, for 0) in reply. 1 found, 0 not, -1 the host cannot be resolved */
int lm_query_info(const char *host, const WORD *ports, int n, WORD game_port, char *reply, int size);
/* the same with another query ("\\status\\": info, rules and players in one reply); any thread */
int lm_query(const char *host, const WORD *ports, int n, WORD game_port, const char *query, char *reply, int size);
int lm_query_value(const char *packet, int len, const char *key, char *value, int size);   /* \key\'s value */

/* the query ports to ask: [Voice] VoiceQueryPorts, else the defaults (the game port + 1 .. ) */
int lm_voice_query_ports(WORD game_port, WORD *ports, int max);
#endif
