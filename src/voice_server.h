/* The game server's voice (voice_server.c): what it needs from the game and what it gives it. */
#ifndef LM_VOICE_SERVER_H
#define LM_VOICE_SERVER_H
#include <stdint.h>

/* a player of the game server, as voice_server_win.c reads it */
typedef struct {
    int id;                          /* the engine's client id (0..255); the voice server's game id is id + 1 */
    char name[64];
    int team;                        /* 1, 2 in team games; 0 in deathmatch (no team radio) */
    int alive;
    int has_position;
    float position[3];               /* world space, LithTech units (LM_UNITS_PER_METRE); Y up, left-handed */
    float forward[3];                /* where it faces (world space), for the stereo pan */
} lm_server_player;

#define LM_SERVER_TEAMS 2
#define LM_UNITS_PER_METRE 52.49f    /* LithTech's tool metric (as in AvP2 and NOLF) */

/* ---------------------------------------------------------------- the game's (voice_server_win.c) */

int lm_server_read_players(lm_server_player *out, int capacity);   /* the count, -1 if the game cannot be read now */
void *lm_server_symbol(const char *name);   /* an export of the Interposer (its voice API) */
uint64_t lm_server_now_ms(void);
const char *lm_server_team_name(int team);  /* "Red", ... as the game names them */

/* ---------------------------------------------------------------- voice_server.c */

void lm_server_tick(void);           /* on the game thread, once a frame */
void lm_server_stop(void);           /* the server shuts down: the voice server stops */
/* the FastDL server the query replies advertise, from the settings (before the replies are sent): 1 if there is one */
int lm_server_fastdl_init(void);
/* the game's query reply with \commsport\<voice port> and the FastDL server's \fastdlport\ (\fastdlhost\,
 * \fastdlpath\) before its \final\ (out holds size bytes): its length, 0 if it is not a reply to change (not one, has
 * the keys already, nothing to add, no room). Any thread. */
int lm_server_query_reply(const char *packet, int length, char *out, int size);
#endif
