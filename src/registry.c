/* [Saves] GameSettings: the game's registry key, HKLM\Software\New World Computing\Legends of Might and Magic\1.0,
 * lives in autoexec.cfg in the save folder instead, as console variables (settings.c), so it is per user and travels
 * with the other saves. Each value has a variable in the game's style:
 *   PlayerName                                   PlayerName
 *   ConnectionSpeed, Port                        ConnectionSpeed, JoinPort (the options menu; sent with the join)
 *   the Host Game screen's last choices          ServerName, ServerMaxPlayers, ServerRoundTime, ServerPurchaseTime,
 *                                                ServerMapTime, ServerFriendlyFire, ServerAllowVoting,
 *                                                ServerBalanceTeams, ServerDedicated, ServerMonsters, ServerLANOnly,
 *                                                ServerLockedChaseCam, ServerSuperWeapons, ServerDamageThroughWalls
 *   Disable Sound, Disable Music                 DisableSound, DisableMusic
 *   Disable Fog, Disable LightMap, Enable OptSurf, DisableFog, DisableLightMap, EnableOptimizeSurfaces,
 *   Enable Tjuncs, Enable TripBuf, Enable         EnableFixTJunctions, EnableTripleBuffer, EnableMultiTexture,
 *   MultiTex, Enable MipSharp                     EnableSharpMipmaps (the launcher's renderer switches; the game turns
 *                                                them into its renderer's variables at startup)
 *   anything else (the launcher's own values)    Launcher<name, letters and digits only>
 * cshell.dll opens the key once (RVA 0x311cb: RegOpenKeyExA, else RegCreateKeyExA, KEY_ALL_ACCESS) and reads and
 * writes REG_SZ and REG_DWORD values through it; those five functions are hooked in its import table and the key is
 * answered from the variables. The first time (none of these variables yet), the values in the real registry (or
 * Windows' VirtualStore copy of them, where LANCommander's install script writes them) are copied over.
 *
 * Numbers (REG_DWORD) are kept in decimal, text (REG_SZ) as it is. Which one the game gets is decided as it asks: its
 * DWORD reads (RVA 0x31256) ask for 4 bytes, and get a number if the value is one. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lommpatch.h"

#define GAME_KEY "Software\\New World Computing\\Legends of Might and Magic\\1.0"
#define FAKE_KEY ((HKEY)(uintptr_t)0x4c4d4d10u)

/* the game's value names and their console variables */
static const struct { const char *value, *cvar; } PLACES[] = {
    {"PlayerName", "PlayerName"},
    {"ConnectionSpeed", "ConnectionSpeed"},
    {"Port", "JoinPort"},
    {"ServerName", "ServerName"},
    {"ServerTotalPlayers", "ServerMaxPlayers"},
    {"ServerRoundTime", "ServerRoundTime"},
    {"ServerPurchaseTime", "ServerPurchaseTime"},
    {"ServerMapTime", "ServerMapTime"},
    {"FriendlyFire", "ServerFriendlyFire"},
    {"AllowVoting", "ServerAllowVoting"},
    {"BalanceTeams", "ServerBalanceTeams"},
    {"DedicatedServer", "ServerDedicated"},
    {"Monsters", "ServerMonsters"},
    {"LanOnly", "ServerLANOnly"},
    {"LockedCam", "ServerLockedChaseCam"},
    {"SuperWeapons", "ServerSuperWeapons"},
    {"DmgThroughWalls", "ServerDamageThroughWalls"},
    {"Disable Sound", "DisableSound"},
    {"Disable Music", "DisableMusic"},
    {"Disable Fog", "DisableFog"},
    {"Disable LightMap", "DisableLightMap"},   /* also spelt "Disable Lightmap": names match regardless of case */
    {"Enable OptSurf", "EnableOptimizeSurfaces"},
    {"Enable Tjuncs", "EnableFixTJunctions"},
    {"Enable TripBuf", "EnableTripleBuffer"},
    {"Enable MultiTex", "EnableMultiTexture"},
    {"Enable MipSharp", "EnableSharpMipmaps"},
};

/* the console variable of a registry value name */
static void cvar_of(const char *name, char *out, int size)
{
    size_t i;
    int n;
    for (i = 0; i < LM_COUNT(PLACES); i++) {
        if (name && !_stricmp(name, PLACES[i].value)) {
            lstrcpynA(out, PLACES[i].cvar, size);
            return;
        }
    }
    n = snprintf(out, size, "Launcher");
    for (; name && *name && n + 1 < size; name++) {
        if (isalnum((unsigned char)*name)) {
            out[n++] = *name;
        }
    }
    out[n] = '\0';
}

static LSTATUS (WINAPI *real_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
static LSTATUS (WINAPI *real_RegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY,
                                             LPDWORD);
static LSTATUS (WINAPI *real_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static LSTATUS (WINAPI *real_RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
static LSTATUS (WINAPI *real_RegCloseKey)(HKEY);

static int is_game_key(HKEY root, LPCSTR path)
{
    return (root == HKEY_LOCAL_MACHINE || root == HKEY_CURRENT_USER) && path && !_stricmp(path, GAME_KEY);
}

/* a whole decimal (or 0x hex) number; "1.000000" (the engine's way of writing a number back) counts as 1 */
static int number(const char *text, DWORD *v)
{
    char *end;
    if (!text[0]) {
        return 0;
    }
    *v = (DWORD)strtoul(text, &end, 0);
    if (*end == '.') {
        for (end++; *end == '0'; end++) {
        }
    }
    return *end == '\0';
}

/* a value: its type, and its bytes in data (n of them); want = the size the game's buffer has */
static int load_value(const char *name, DWORD want, DWORD *type, BYTE *data, DWORD *n)
{
    char text[256], cvar[64];
    DWORD v;
    cvar_of(name, cvar, sizeof cvar);
    if (!lm_setting_get(cvar, text, sizeof text)) {
        return 0;
    }
    if (want == 4 && number(text, &v)) {
        *type = REG_DWORD;
        memcpy(data, &v, 4);
        *n = 4;
    } else {
        *type = REG_SZ;
        *n = (DWORD)strlen(text) + 1;
        memcpy(data, text, *n);
    }
    return 1;
}

static void store_value(const char *name, DWORD type, const BYTE *data, DWORD n)
{
    char text[256], cvar[64];
    cvar_of(name, cvar, sizeof cvar);
    if (type == REG_DWORD && n >= 4) {
        DWORD v;
        memcpy(&v, data, 4);
        snprintf(text, sizeof text, "%lu", v);
    } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        int len = (int)(n && data[n - 1] == 0 ? n - 1 : n);
        if (len > 250) {
            len = 250;
        }
        snprintf(text, sizeof text, "%.*s", len, (const char *)data);   /* quotes and line breaks are dropped */
    } else {
        lm_log("registry: %s is of type %lu, which the game does not use; not kept", name ? name : "@", type);
        return;
    }
    lm_setting_set(cvar, text);
}

/* the real key's values, while autoexec.cfg has none of the game's */
static void import_real(void)
{
    char text[8];
    HKEY k;
    DWORD i;
    for (i = 0; i < LM_COUNT(PLACES); i++) {
        if (lm_setting_get(PLACES[i].cvar, text, sizeof text)) {
            return;                  /* imported already */
        }
    }
    if (real_RegOpenKeyExA(HKEY_LOCAL_MACHINE, GAME_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        lm_log("registry: no %s in the registry, starting empty", GAME_KEY);
        return;
    }
    for (i = 0;; i++) {
        char name[256];
        BYTE data[1024];
        DWORD nl = sizeof name, dl = sizeof data, type;
        LSTATUS r = RegEnumValueA(k, i, name, &nl, NULL, &type, data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (r == ERROR_SUCCESS) {
            store_value(name, type, data, dl);
        }
    }
    real_RegCloseKey(k);
    lm_log("registry: copied %lu values of the game's registry key into autoexec.cfg", i);
}

static LSTATUS WINAPI hk_RegOpenKeyExA(HKEY root, LPCSTR path, DWORD opts, REGSAM sam, PHKEY out)
{
    if (is_game_key(root, path) && out) {
        *out = FAKE_KEY;
        return ERROR_SUCCESS;
    }
    return real_RegOpenKeyExA(root, path, opts, sam, out);
}

static LSTATUS WINAPI hk_RegCreateKeyExA(HKEY root, LPCSTR path, DWORD reserved, LPSTR cls, DWORD opts, REGSAM sam,
                                        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
    if (is_game_key(root, path) && out) {
        *out = FAKE_KEY;
        if (disp) {
            *disp = REG_OPENED_EXISTING_KEY;
        }
        return ERROR_SUCCESS;
    }
    return real_RegCreateKeyExA(root, path, reserved, cls, opts, sam, sa, out, disp);
}

static LSTATUS WINAPI hk_RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                                         LPDWORD size)
{
    BYTE value[1024];
    DWORD t, n;
    if (key != FAKE_KEY) {
        return real_RegQueryValueExA(key, name, reserved, type, data, size);
    }
    if (!load_value(name, data && size ? *size : 0, &t, value, &n)) {
        return ERROR_FILE_NOT_FOUND;
    }
    if (type) {
        *type = t;
    }
    if (!size) {
        return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    }
    if (data && *size < n) {
        *size = n;
        return ERROR_MORE_DATA;
    }
    if (data) {
        memcpy(data, value, n);
    }
    *size = n;
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI hk_RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD n)
{
    if (key != FAKE_KEY) {
        return real_RegSetValueExA(key, name, reserved, type, data, n);
    }
    store_value(name, type, data, data ? n : 0);
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI hk_RegCloseKey(HKEY key)
{
    return key == FAKE_KEY ? ERROR_SUCCESS : real_RegCloseKey(key);
}

#define HOOK(m, name) do { void *old = lm_hook_import_in(m, "ADVAPI32.dll", #name, hk_##name); \
                           if (!real_##name) real_##name = old ? old : (void *)name; } while (0)

static void on_cshell(HMODULE m)
{
    HOOK(m, RegOpenKeyExA);
    HOOK(m, RegCreateKeyExA);
    HOOK(m, RegQueryValueExA);
    HOOK(m, RegSetValueExA);
    HOOK(m, RegCloseKey);
    import_real();
    lm_log("registry: the game's settings key is kept in autoexec.cfg");
}

void lm_registry_install(void)
{
    if (!lm_ini_int("Saves", "GameSettings", 1) || !lm_save_dir()[0]) {
        return;
    }
    lm_on_module(LM_MOD_CSHELL, on_cshell);
}
