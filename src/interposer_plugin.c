/* LANCommander Interposer plugin build (LANCommander.Interposer.Plugin.LegendsOfMightAndMagic.dll).
 *
 * The Interposer loads every DLL in .interposer\Plugins\ from its own DLL_PROCESS_ATTACH, after its hooks are
 * installed, and calls InterposerPluginInit with its module handle. With the Interposer deployed as a proxy DLL
 * (dinput.dll, version.dll) or through the injector's --launch mode this is before lithtech.exe (or LegendServ.exe)
 * runs any of its own code. The same patches as the dinput.dll proxy are applied; settings come from
 * Plugins.LegendsOfMightAndMagic in .interposer\Config.yml (defaults are added on first run), and log lines go to the
 * Interposer's session log as [LOMM]. The DirectInput fix and FastDL are the Interposer's own settings there
 * (DirectInput.FixLegacyDeviceEnumeration, FastDL.*): the plugin does not write them.
 *
 * Interposer exports are __cdecl (declared without a convention in its config.h); only InterposerPluginInit itself
 * is WINAPI.
 *
 * InterposerGetConfigString is only used when the Interposer also exports InterposerSetConfigString (added in
 * eb42c5a, 2026-10-18). In older builds its path walk assigned each child over its parent, so the first lookup
 * clobbered the in-memory config root: later lookups (ours and the Interposer's own) fail. With those builds the
 * plugin reads its section of Config.yml itself instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "lommpatch.h"

#define SECTION "LegendsOfMightAndMagic"

typedef void (*InterposerLog_fn)(const wchar_t *verb, const wchar_t *message);
typedef BOOL (*InterposerGetConfigString_fn)(const wchar_t *dot_path, wchar_t *buffer, DWORD size);
typedef BOOL (*InterposerRegisterPluginConfig_fn)(const wchar_t *plugin_name, const wchar_t *yaml_defaults);

static InterposerLog_fn g_log;
static InterposerGetConfigString_fn g_get_config;   /* NULL with Interposer builds that predate the fix */

/* Written to Config.yml only when there is no Plugins.LegendsOfMightAndMagic section yet; a missing key falls back to
 * the built-in default in the code, which is the same value. One map per section of lommpatch.ini, with the same
 * keys: lommpatch.ini's [Voice] Port is Plugins.LegendsOfMightAndMagic.Voice.Port. The DirectInput fix ([Input]) and
 * FastDL's download side are the Interposer's own settings, not these. */
static const wchar_t DEFAULTS[] =
    L"General:\n"
    L"  Enabled: true\n"
    L"Saves:\n"
    L"  Redirect: true\n"
    L"  Folder: \"\"\n"
    L"  GameSettings: true\n"
    L"Display:\n"
    L"  HorPlus: true\n"
    L"  ScaleMenus: true\n"
    L"  ScaleHud: true\n"
    L"  HudScale: 0\n"
    L"  IntegerScale: false\n"
    L"  Borderless: false\n"
    L"Performance:\n"
    L"  TimerResolution: true\n"
    L"  PreciseLimiter: true\n"
    L"  FixMenus: true\n"
    L"  FixConsole: true\n"
    L"Network:\n"
    L"  QueryPorts: \"\"\n"
    L"Voice:\n"
    L"  Enabled: true\n"
    L"  Port: 27800\n"
    L"  Password: \"\"\n"
    L"  Host: \"\"\n"
    L"  VoiceActivation: false\n"
    L"  Volume: 100\n"
    L"  MicVolume: 100\n"
    L"  Microphone: \"\"\n"
    L"  ShowTalkers: true\n"
    L"FastDL:\n"
    L"  Enabled: true\n"
    L"  Url: \"\"\n"
    L"RichPresence:\n"
    L"  Enabled: true\n"
    L"  LargeImage: \"\"\n"
    L"Server:\n"
    L"  VoiceServer: true\n"
    L"  VoiceRange: \"\"\n"
    L"  VoiceFullVolumeRange: \"\"\n"
    L"  VoiceMaxPlayers: 32\n"
    L"  VoiceMaxBitrate: 32000\n"
    L"  VoiceBindAddress: \"\"\n"
    L"  VoiceAdminKey: \"\"\n"
    L"  FastDLPort: 0\n"
    L"  FastDLHost: \"\"\n"
    L"  FastDLPath: \"\"\n"
    L"Debug:\n"
    L"  CrashLog: false\n";

/* Plugins.LegendsOfMightAndMagic from Config.yml, for Interposer builds whose InterposerGetConfigString is unusable */
#define MAX_KEYS 96
static struct { char key[64]; char value[MAX_PATH]; } g_keys[MAX_KEYS];
static int g_nkeys;

static int indent_of(const char *line)
{
    int n = 0;
    while (line[n] == ' ') {
        n++;
    }
    return n;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

/* block-style "Plugins:" / "  LegendsOfMightAndMagic:" / "    Section:" / "      Key: value" (what
 * InterposerRegisterPluginConfig writes); each key is kept as "Section.Key" */
static void read_section(const wchar_t *config_path)
{
    char line[512];
    int in_plugins = 0, section_indent = -1, map_indent = -1;
    char map[32] = "";
    FILE *f = _wfopen(config_path, L"r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof line, f) && g_nkeys < MAX_KEYS) {
        char *text = line, *colon, *value, *hash;
        int indent;
        if (!strncmp(text, "\xef\xbb\xbf", 3)) {
            text += 3;
        }
        trim(text);
        indent = indent_of(text);
        text += indent;
        if (!*text || *text == '#') {
            continue;
        }
        if (indent == 0) {
            in_plugins = !strcmp(text, "Plugins:");
            section_indent = -1;
            continue;
        }
        if (!in_plugins) {
            continue;
        }
        if (section_indent >= 0 && indent <= section_indent) {
            section_indent = -1;               /* left Plugins.LegendsOfMightAndMagic */
        }
        if (section_indent < 0) {
            if (!strcmp(text, SECTION ":")) {
                section_indent = indent;
                map_indent = -1;
                map[0] = '\0';
            }
            continue;
        }
        if (map_indent >= 0 && indent <= map_indent) {
            map_indent = -1;                   /* left Plugins.LegendsOfMightAndMagic.<map> */
            map[0] = '\0';
        }
        colon = strchr(text, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        value = colon + 1;
        while (*value == ' ') {
            value++;
        }
        if (!*value && map_indent < 0) {
            map_indent = indent;               /* "Section:" opens a map */
            lstrcpynA(map, text, sizeof map);
            continue;
        }
        if (*value == '"' || *value == '\'') {
            char *end = strchr(value + 1, *value);
            if (end) {
                *end = '\0';
            }
            value++;
        } else if ((hash = strstr(value, " #")) != NULL) {
            *hash = '\0';
        }
        trim(text);
        trim(value);
        snprintf(g_keys[g_nkeys].key, sizeof g_keys[0].key, "%s%s%s", map, map[0] ? "." : "", text);
        lstrcpynA(g_keys[g_nkeys].value, value, sizeof g_keys[0].value);
        g_nkeys++;
    }
    fclose(f);
}

static int get_value(const char *section, const char *key, wchar_t *value, int size)
{
    char path_a[96];
    int i;
    snprintf(path_a, sizeof path_a, "%s.%s", section, key);
    if (g_get_config) {
        wchar_t path[128];
        swprintf(path, sizeof path / sizeof path[0], L"Plugins." SECTION ".%hs", path_a);
        return g_get_config(path, value, (DWORD)size);
    }
    for (i = 0; i < g_nkeys; i++) {
        if (!_stricmp(g_keys[i].key, path_a)) {
            return MultiByteToWideChar(CP_UTF8, 0, g_keys[i].value, -1, value, size) > 0;
        }
    }
    return 0;
}

/* lommpatch.ini's [Section] Key -> Plugins.LegendsOfMightAndMagic.Section.Key */
static int config_int(const char *section, const char *key, int def)
{
    wchar_t value[64];
    wchar_t *end;
    long n;
    if (!get_value(section, key, value, sizeof value / sizeof value[0])) {
        return def;
    }
    if (!_wcsicmp(value, L"true") || !_wcsicmp(value, L"yes") || !_wcsicmp(value, L"on")) {
        return 1;
    }
    if (!_wcsicmp(value, L"false") || !_wcsicmp(value, L"no") || !_wcsicmp(value, L"off")) {
        return 0;
    }
    n = wcstol(value, &end, 0);
    if (end == value) {
        lm_log("config: Plugins." SECTION ".%s.%s = \"%ls\" is not a number or true/false, using %d", section, key, value, def);
        return def;
    }
    return (int)n;
}

static int config_str(const char *section, const char *key, char *out, int size)
{
    wchar_t value[MAX_PATH];
    if (!get_value(section, key, value, MAX_PATH)) {
        return 0;
    }
    return WideCharToMultiByte(CP_ACP, 0, value, -1, out, size, NULL, NULL) > 1;
}

static void log_line(const char *line)
{
    wchar_t wide[1024];
    if (g_log && MultiByteToWideChar(CP_ACP, 0, line, -1, wide, sizeof wide / sizeof wide[0]) > 0) {
        g_log(L"LOMM", wide);
    }
}

void WINAPI InterposerPluginInit(HMODULE interposer)
{
    InterposerRegisterPluginConfig_fn register_config;
    g_log = (InterposerLog_fn)(void (*)(void))GetProcAddress(interposer, "InterposerLog");
    register_config = (InterposerRegisterPluginConfig_fn)(void (*)(void))GetProcAddress(interposer,
                                                                                        "InterposerRegisterPluginConfig");
    if (register_config) {
        register_config(L"" SECTION, DEFAULTS);
    }
    lm_set_module(GetModuleHandleA("LANCommander.Interposer.Plugin.LegendsOfMightAndMagic.dll"));
    lm_set_backend(config_int, log_line);
    lm_set_string_backend(config_str);
    lm_voice_set_interposer(interposer);

    if (GetProcAddress(interposer, "InterposerSetConfigString")) {
        g_get_config = (InterposerGetConfigString_fn)(void (*)(void))GetProcAddress(interposer,
                                                                                   "InterposerGetConfigString");
    }
    if (!g_get_config) {
        wchar_t path[MAX_PATH + 32], *slash;
        GetModuleFileNameW(interposer, path, MAX_PATH);
        slash = wcsrchr(path, L'\\');
        if (slash) {
            slash[1] = L'\0';
        }
        wcscat(path, L".interposer\\Config.yml");
        read_section(path);
        lm_log("config: older Interposer, read %d Plugins." SECTION " settings from %ls", g_nkeys, path);
    }

    if (lm_patch_start()) {
        lm_install();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
