/* The player's settings: console variables in autoexec.cfg (in the save folder), like the game's own.
 *
 * Every setting of the patch has a console variable name in the game's style ("HorPlus", "VoiceVolume", ...; the
 * table below). lm_ini_int / lm_ini_str (lommpatch.c) look a setting up:
 *   1. as a console variable of the running engine, once the client shell is loaded (autoexec.cfg has been run then,
 *      and "VoiceVolume 50" typed in the console counts);
 *   2. in autoexec.cfg itself, read by the patch before the engine starts;
 *   3. in lommpatch.ini next to the patch (or the Interposer's Config.yml for the plugin build): the defaults.
 * [General] and [Saves] are read from lommpatch.ini only (they decide where autoexec.cfg is). The first time, the
 * settings missing from autoexec.cfg are written into it with their defaults (from lommpatch.ini, else built in), so
 * they can be seen and changed there. A change from the patch (its options in the game's menus, the registry values
 * of registry.c) goes to the engine's variable (with its save flag, "+Name value") and straight into autoexec.cfg.
 * The engine rewrites autoexec.cfg itself when an options screen's OK is pressed and when it quits, with every
 * variable that has the save flag; those it read from the file at startup have it.
 *
 * Patch versions up to 1.0.x kept these in Settings.ini in the save folder: its values are moved into autoexec.cfg
 * once and the file is renamed Settings.ini.old.
 *
 * The dedicated server (LegendServ.exe) has no autoexec.cfg of the player's: it reads lommpatch.ini only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lommpatch.h"

#define ENGINE_GET_VAR   0x4084c2u   /* cdecl HCONSOLEVAR (const char *name): ILTClient GetConsoleVar */
#define ENGINE_RUN       0x40887bu   /* cdecl void (const char *command): ILTClient RunConsoleString */
#define VAR_STRING       0x14        /* const char * */

enum { F_PATCH = 1, F_GAME = 2 };    /* a setting of the patch's; a value of the game's registry key (registry.c) */

static const struct setting {
    const char *section, *key, *cvar, *def;
    int flags;
} SETTINGS[] = {
    {"Display", "HorPlus", "HorPlus", "1", F_PATCH},
    {"Display", "ScaleMenus", "ScaleMenus", "1", F_PATCH},
    {"Display", "ScaleHud", "ScaleHUD", "1", F_PATCH},
    {"Display", "HudScale", "HUDScale", "0", F_PATCH},
    {"Display", "IntegerScale", "IntegerScale", "0", F_PATCH},
    {"Display", "Borderless", "Borderless", "0", F_PATCH},
    {"Performance", "TimerResolution", "TimerResolution", "1", F_PATCH},
    {"Performance", "PreciseLimiter", "PreciseFrameLimiter", "1", F_PATCH},
    {"Performance", "FixMenus", "FastMenus", "1", F_PATCH},
    {"Performance", "FixConsole", "FastConsole", "1", F_PATCH},
    {"Input", "FixDirectInput", "FixDirectInput", "1", F_PATCH},
    {"Input", "KeyboardMouseOnly", "KeyboardMouseOnly", "0", F_PATCH},
    {"Network", "QueryPorts", "ServerQueryPorts", "", F_PATCH},
    {"Voice", "Enabled", "VoiceEnable", "1", F_PATCH},
    {"Voice", "Port", "VoicePort", "27800", F_PATCH},
    {"Voice", "Password", "VoicePassword", "", F_PATCH},
    {"Voice", "Host", "VoiceHost", "", F_PATCH},
    {"Voice", "VoiceActivation", "VoiceActivation", "0", F_PATCH},
    {"Voice", "Volume", "VoiceVolume", "100", F_PATCH},
    {"Voice", "MicVolume", "VoiceMicVolume", "100", F_PATCH},
    {"Voice", "Microphone", "VoiceMicrophone", "", F_PATCH},
    {"Voice", "ShowTalkers", "VoiceShowTalkers", "1", F_PATCH},
    {"FastDL", "Enabled", "FastDLEnable", "1", F_PATCH},
    {"FastDL", "Url", "FastDLURL", "", F_PATCH},
    {"RichPresence", "Enabled", "RichPresenceEnable", "1", F_PATCH},
    {"RichPresence", "DiscordApplicationId", "DiscordApplicationID", "", F_PATCH},
    {"RichPresence", "LargeImage", "DiscordLargeImage", "", F_PATCH},
    {"Server", "VoiceServer", "VoiceServer", "1", F_PATCH},
    {"Server", "VoiceRange", "VoiceRange", "", F_PATCH},
    {"Server", "VoiceFullVolumeRange", "VoiceFullVolumeRange", "", F_PATCH},
    {"Server", "VoiceMaxPlayers", "VoiceMaxPlayers", "32", F_PATCH},
    {"Server", "VoiceMaxBitrate", "VoiceMaxBitrate", "32000", F_PATCH},
    {"Server", "VoiceBindAddress", "VoiceBindAddress", "", F_PATCH},
    {"Server", "VoiceAdminKey", "VoiceAdminKey", "", F_PATCH},
    {"Server", "FastDLPort", "FastDLPort", "0", F_PATCH},
    {"Server", "FastDLHost", "FastDLHost", "", F_PATCH},
    {"Server", "FastDLPath", "FastDLPath", "", F_PATCH},
    {"Debug", "CrashLog", "CrashLog", "0", F_PATCH},
    {"Debug", "PerfLog", "PerfLog", "0", F_PATCH},
    /* the game's registry values (registry.c), under their Settings.ini names of patch 1.0.x */
    {"Player", "Name", "PlayerName", NULL, F_GAME},
    {"Network", "ConnectionSpeed", "ConnectionSpeed", NULL, F_GAME},
    {"Network", "Port", "JoinPort", NULL, F_GAME},
    {"Server", "Name", "ServerName", NULL, F_GAME},
    {"Server", "MaxPlayers", "ServerMaxPlayers", NULL, F_GAME},
    {"Server", "RoundTime", "ServerRoundTime", NULL, F_GAME},
    {"Server", "PurchaseTime", "ServerPurchaseTime", NULL, F_GAME},
    {"Server", "MapTime", "ServerMapTime", NULL, F_GAME},
    {"Server", "FriendlyFire", "ServerFriendlyFire", NULL, F_GAME},
    {"Server", "AllowVoting", "ServerAllowVoting", NULL, F_GAME},
    {"Server", "BalanceTeams", "ServerBalanceTeams", NULL, F_GAME},
    {"Server", "Dedicated", "ServerDedicated", NULL, F_GAME},
    {"Server", "Monsters", "ServerMonsters", NULL, F_GAME},
    {"Server", "LanOnly", "ServerLANOnly", NULL, F_GAME},
    {"Server", "LockedChaseCam", "ServerLockedChaseCam", NULL, F_GAME},
    {"Server", "SuperWeapons", "ServerSuperWeapons", NULL, F_GAME},
    {"Server", "DamageThroughWalls", "ServerDamageThroughWalls", NULL, F_GAME},
    {"Sound", "DisableSound", "DisableSound", NULL, F_GAME},
    {"Sound", "DisableMusic", "DisableMusic", NULL, F_GAME},
    {"Display", "DisableFog", "DisableFog", NULL, F_GAME},
    {"Display", "DisableLightMap", "DisableLightMap", NULL, F_GAME},
    {"Display", "OptimizeSurfaces", "EnableOptimizeSurfaces", NULL, F_GAME},
    {"Display", "FixTJunctions", "EnableFixTJunctions", NULL, F_GAME},
    {"Display", "TripleBuffer", "EnableTripleBuffer", NULL, F_GAME},
    {"Display", "MultiTexture", "EnableMultiTexture", NULL, F_GAME},
    {"Display", "SharpMipmaps", "EnableSharpMipmaps", NULL, F_GAME},
};

/* autoexec.cfg's variables, as the patch read (and wrote) them */
typedef struct { char name[64]; char value[256]; } var_t;
static var_t *g_vars;
static int g_nvars, g_cap;
static char g_file[MAX_PATH];
static int g_state;                  /* 0 = not loaded, 1 = loading, 2 = loaded (or nothing to load) */
static CRITICAL_SECTION g_cs;

static const struct setting *by_key(const char *section, const char *key)
{
    size_t i;
    for (i = 0; i < LM_COUNT(SETTINGS); i++) {
        if (!_stricmp(SETTINGS[i].section, section) && !_stricmp(SETTINGS[i].key, key)) {
            return &SETTINGS[i];
        }
    }
    return NULL;
}

static var_t *find(const char *name)
{
    int i;
    for (i = 0; i < g_nvars; i++) {
        if (!_stricmp(g_vars[i].name, name)) {
            return &g_vars[i];
        }
    }
    return NULL;
}

static void put(const char *name, const char *value)
{
    var_t *v = find(name);
    if (!v) {
        if (g_nvars == g_cap) {
            int cap = g_cap ? g_cap * 2 : 128;
            var_t *n = realloc(g_vars, cap * sizeof *n);
            if (!n) {
                return;
            }
            g_vars = n;
            g_cap = cap;
        }
        v = &g_vars[g_nvars++];
        lstrcpynA(v->name, name, sizeof v->name);
    }
    lstrcpynA(v->value, value, sizeof v->value);
}

/* the next token of a console line (quoted or not); NULL at the end */
static const char *token(const char *p, char *out, int size)
{
    int n = 0;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (!*p || *p == '\r' || *p == '\n' || *p == ';' || (p[0] == '/' && p[1] == '/')) {
        return NULL;
    }
    if (*p == '"') {
        for (p++; *p && *p != '"' && *p != '\r' && *p != '\n'; p++) {
            if (n + 1 < size) out[n++] = *p;
        }
        if (*p == '"') {
            p++;
        }
    } else {
        for (; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'; p++) {
            if (n + 1 < size) out[n++] = *p;
        }
    }
    out[n] = '\0';
    return p;
}

/* a variable assignment ("Name" "value": exactly two tokens) */
static int assignment(const char *line, char *name, int nsize, char *value, int vsize)
{
    char extra[8];
    const char *p = token(line, name, nsize);
    if (!p || !(p = token(p, value, vsize))) {
        return 0;
    }
    return token(p, extra, sizeof extra) == NULL;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *s;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || !(s = malloc((size_t)n + 1))) {
        fclose(f);
        return NULL;
    }
    n = (long)fread(s, 1, (size_t)n, f);
    s[n] = '\0';
    fclose(f);
    return s;
}

/* autoexec.cfg with name's assignment replaced (or added at the end); the lines are kept as they are otherwise */
static void write_var(const char *name, const char *value)
{
    char *old = read_file(g_file), *out, tmp[MAX_PATH + 8], line_name[64], line_value[8];
    const char *p, *eol;
    size_t len = old ? strlen(old) : 0, n = 0, cap = len + strlen(name) + strlen(value) + 16;
    int done = 0;
    FILE *f;
    if (!(out = malloc(cap))) {
        free(old);
        return;
    }
    for (p = old; p && *p; p = eol) {
        eol = strchr(p, '\n');
        eol = eol ? eol + 1 : p + strlen(p);
        if (!done && assignment(p, line_name, sizeof line_name, line_value, sizeof line_value)
            && !_stricmp(line_name, name)) {
            n += (size_t)sprintf(out + n, "\"%s\" \"%s\"\r\n", name, value);
            done = 1;
            continue;
        }
        memcpy(out + n, p, (size_t)(eol - p));
        n += (size_t)(eol - p);
    }
    if (!done) {
        if (n && out[n - 1] != '\n') {
            out[n++] = '\r';
            out[n++] = '\n';
        }
        n += (size_t)sprintf(out + n, "\"%s\" \"%s\"\r\n", name, value);
    }
    snprintf(tmp, sizeof tmp, "%s.new", g_file);
    if ((f = fopen(tmp, "wb")) != NULL) {
        int ok = fwrite(out, 1, n, f) == n;
        ok = !fclose(f) && ok;
        if (!ok || !MoveFileExA(tmp, g_file, MOVEFILE_REPLACE_EXISTING)) {
            lm_log("settings: cannot write %s (%lu)", g_file, GetLastError());
            DeleteFileA(tmp);
        }
    }
    free(out);
    free(old);
}

/* a value without the characters a console line cannot hold */
static void clean(const char *in, char *out, int size)
{
    int n = 0;
    for (; *in && n + 1 < size; in++) {
        if (*in != '"' && *in != '\r' && *in != '\n') {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
}

static int engine_up(void)
{
    static const BYTE get_head[] = {0xff, 0x74, 0x24, 0x04, 0x68, 0xa0, 0x34, 0x49, 0x00};
    static const BYTE run_head[] = {0xff, 0x74, 0x24, 0x04, 0xe8};
    static int ok = -1;
    if (lm_process() != LM_PROC_CLIENT || !lm_game_module(LM_MOD_CSHELL)) {
        return 0;
    }
    if (ok < 0) {
        ok = lm_expect(ENGINE_GET_VAR, get_head, sizeof get_head) && lm_expect(ENGINE_RUN, run_head, sizeof run_head);
        if (!ok) {
            lm_log("settings: the engine's console functions differ; settings are read from autoexec.cfg only");
        }
    }
    return ok;
}

/* migrate Settings.ini of patch 1.0.x into autoexec.cfg */
static void migrate_settings_ini(void)
{
    char ini[MAX_PATH], old[MAX_PATH + 8], value[256];
    size_t i;
    int n = 0;
    snprintf(ini, sizeof ini, "%sSettings.ini", lm_save_dir());
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    for (i = 0; i < LM_COUNT(SETTINGS); i++) {
        GetPrivateProfileStringA(SETTINGS[i].section, SETTINGS[i].key, "\x01", value, sizeof value, ini);
        if (strcmp(value, "\x01") && !find(SETTINGS[i].cvar)) {
            char v[256];
            clean(value, v, sizeof v);
            put(SETTINGS[i].cvar, v);
            write_var(SETTINGS[i].cvar, v);
            n++;
        }
    }
    snprintf(old, sizeof old, "%s.old", ini);
    MoveFileExA(ini, old, MOVEFILE_REPLACE_EXISTING);
    lm_log("settings: moved %d values of %s into autoexec.cfg (the file is now Settings.ini.old)", n, ini);
}

static void load(void)
{
    char *text, name[64], value[256];
    const char *p, *eol;
    size_t i;
    int added = 0;
    if (lm_process() != LM_PROC_CLIENT) {
        g_state = 2;
        return;
    }
    if (!lm_save_dir_ready()) {
        g_state = 0;                 /* asked while the save folder is being worked out: later */
        return;
    }
    snprintf(g_file, sizeof g_file, "%sautoexec.cfg", lm_save_dir()[0] ? lm_save_dir() : lm_game_dir());
    lm_saves_prepare();              /* the install folder's autoexec.cfg copied over the first time */
    if ((text = read_file(g_file)) != NULL) {
        for (p = text; *p; p = eol) {
            eol = strchr(p, '\n');
            eol = eol ? eol + 1 : p + strlen(p);
            if (assignment(p, name, sizeof name, value, sizeof value)) {
                put(name, value);
            }
        }
        free(text);
    }
    if (lm_save_dir()[0]) {
        migrate_settings_ini();
    }
    /* the patch's settings missing from autoexec.cfg, with their defaults (lommpatch.ini's, else built in) */
    for (i = 0; i < LM_COUNT(SETTINGS); i++) {
        const struct setting *s = &SETTINGS[i];
        char def[256];
        if (!(s->flags & F_PATCH) || find(s->cvar)) {
            continue;
        }
        if (!lm_ini_default(s->section, s->key, def, sizeof def)) {
            lstrcpynA(def, s->def, sizeof def);
        } else if (!_stricmp(def, "true") || !_stricmp(def, "false")) {
            strcpy(def, def[0] == 't' || def[0] == 'T' ? "1" : "0");   /* Config.yml's booleans */
        }
        clean(def, value, sizeof value);
        if (!value[0]) {
            continue;                /* nothing to say: left out */
        }
        put(s->cvar, value);
        write_var(s->cvar, value);
        added++;
    }
    if (added) {
        lm_log("settings: added %d settings of the patch to %s", added, g_file);
    }
    g_state = 2;
}

static int ensure_loaded(void)
{
    static LONG init;
    if (InterlockedCompareExchange(&init, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        InterlockedExchange(&init, 2);
    }
    while (init != 2) {
        Sleep(0);
    }
    if (g_state == 1) {
        return 0;                    /* asked from within the loading (the save folder's own lookups) */
    }
    if (g_state == 0) {
        EnterCriticalSection(&g_cs);
        if (g_state == 0) {
            g_state = 1;
            load();
        }
        LeaveCriticalSection(&g_cs);
    }
    return g_state == 2;
}

int lm_setting_get(const char *cvar, char *out, int size)
{
    var_t *v;
    out[0] = '\0';
    if (!ensure_loaded()) {
        return 0;
    }
    if (engine_up()) {
        void *var = ((void *(__cdecl *)(const char *))LM_ADDR(ENGINE_GET_VAR))(cvar);
        const char *s = var ? *(const char **)((BYTE *)var + VAR_STRING) : NULL;
        if (s && lm_readable(s, 1)) {
            lstrcpynA(out, s, size);
            return 1;
        }
    }
    EnterCriticalSection(&g_cs);
    if ((v = find(cvar)) != NULL) {
        lstrcpynA(out, v->value, size);
    }
    LeaveCriticalSection(&g_cs);
    return v != NULL;
}

void lm_setting_set(const char *cvar, const char *value)
{
    char v[256], cmd[400];
    if (!ensure_loaded() || !g_file[0]) {
        return;
    }
    clean(value, v, sizeof v);
    EnterCriticalSection(&g_cs);
    put(cvar, v);
    write_var(cvar, v);
    LeaveCriticalSection(&g_cs);
    if (engine_up()) {
        /* "+": the variable gets the engine's save flag, so its own writes of autoexec.cfg (options OK, quitting:
         * 0x415e31 writes flagged variables only) keep it. Those read from autoexec.cfg at startup have it already. */
        snprintf(cmd, sizeof cmd, "+%s \"%s\"", cvar, v);
        ((void (__cdecl *)(const char *))LM_ADDR(ENGINE_RUN))(cmd);
    }
}

void lm_setting_set_int(const char *cvar, int value)
{
    char text[16];
    snprintf(text, sizeof text, "%d", value);
    lm_setting_set(cvar, text);
}

int lm_setting_int(const char *cvar, int def)
{
    char text[64], *end;
    long n;
    if (!lm_setting_get(cvar, text, sizeof text)) {
        return def;
    }
    n = strtol(text, &end, 10);
    if (end == text) {               /* "1.000000", as the engine writes floats, is still 1 */
        return def;
    }
    return (int)n;
}

/* lommpatch.c: the player's value of [section] key, if it is one of these settings */
int lm_settings_lookup(const char *section, const char *key, char *out, int size)
{
    const struct setting *s;
    if (!_stricmp(section, "Saves") || !_stricmp(section, "General") || !(s = by_key(section, key))) {
        return 0;
    }
    return lm_setting_get(s->cvar, out, size);
}
