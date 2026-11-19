/* The embedded LANCommander Interposer's .interposer\Config.yml (dinput.dll / ASI builds), next to the patch, written
 * before the Interposer starts (it reads it then) when there is none, or an empty one; one that is there already is
 * the player's or LANCommander's, and is left alone. It turns on:
 * - DirectInput.FixLegacyDeviceEnumeration ([Input] FixDirectInput): the game asks for DirectInput 7, whose dinput.dll
 *   on modern Windows writes past its device array while it enumerates HID devices, so with enough of them plugged in
 *   the game hangs or crashes at startup. The Interposer bridges the game to DirectInput 8 instead.
 *   [Input] KeyboardMouseOnly=1 also shows the game only the keyboard and mouse (no enumeration at all).
 * - FastDL ([FastDL] Enabled) for the worlds under Data\Worlds: a world the game lacks is downloaded from the FastDL
 *   server of the game server it joins (fastdl.c tells the Interposer where that is).
 * - RichPresence.Discord ([RichPresence] DiscordApplicationId): the Interposer connects to Discord only at startup,
 *   with the application id in its Config.yml, and shows what presence.c sets. Without an id the presence still goes
 *   to the LANCommander launcher. This block is also added to a Config.yml that is there already but has no
 *   RichPresence section, so setting the id later works. */
#include <stdio.h>
#include <string.h>
#include "lommpatch.h"

/* the RichPresence block, if there is a Discord application id ([RichPresence] DiscordApplicationId) */
static int write_presence(FILE *f)
{
    char id[64];
    const char *c;
    if (!lm_ini_int("RichPresence", "Enabled", 1) || !lm_ini_str("RichPresence", "DiscordApplicationId", id, sizeof id)) {
        return 0;
    }
    for (c = id; *c; c++) {
        if (*c < '0' || *c > '9') {
            lm_log("interposer: [RichPresence] DiscordApplicationId=%s is not a number, no Discord", id);
            return 0;
        }
    }
    fprintf(f,
            "RichPresence:\n"
            "  # what the player is doing, in Discord (set by the patch while the game runs)\n"
            "  Discord:\n"
            "    Enabled: true\n"
            "    ApplicationId: \"%s\"\n"
            "  Name: \"Legends of Might and Magic\"\n"
            "  Details: \"In the menus\"\n",
            id);
    return 1;
}

/* a Config.yml with no top-level RichPresence gets one (appended: the rest of the file is left as it is) */
static void add_presence(const char *path)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int found = 0, last_newline = 1;
    if (!f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        const char *text = !strncmp(line, "\xef\xbb\xbf", 3) ? line + 3 : line;
        size_t n = strlen(line);
        if (!strncmp(text, "RichPresence:", 13)) {
            found = 1;
        }
        last_newline = n && line[n - 1] == '\n';
    }
    fclose(f);
    if (found || !(f = fopen(path, "a"))) {
        return;
    }
    if (!last_newline) {
        fputc('\n', f);
    }
    if (write_presence(f)) {
        lm_log("interposer: added RichPresence (Discord) to %s", path);
    }
    fclose(f);
}

void lm_interposer_write_config(HMODULE dll)
{
    char path[MAX_PATH + 32], *slash;
    WIN32_FILE_ATTRIBUTE_DATA a;
    int dinput = lm_ini_int("Input", "FixDirectInput", 1), kbm = lm_ini_int("Input", "KeyboardMouseOnly", 0);
    int fastdl = lm_ini_int("FastDL", "Enabled", 1), discord;
    FILE *f;
    if (!GetModuleFileNameA(dll, path, MAX_PATH) || !(slash = strrchr(path, '\\'))) {
        return;
    }
    strcpy(slash + 1, ".interposer\\Config.yml");
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a) && (a.nFileSizeLow || a.nFileSizeHigh)) {
        lm_log("interposer: using %s as it is", path);
        add_presence(path);
        return;
    }
    lm_make_dirs(path);
    if (!(f = fopen(path, "w"))) {
        lm_log("interposer: cannot write %s; the DirectInput fix and FastDL stay off", path);
        return;
    }
    fputs("# Written by lommpatch (the Legends of Might and Magic patch) for the LANCommander Interposer built into it.\n"
          "# Edit freely: the patch only writes this file when there is none. Every option is documented in the\n"
          "# Interposer's sample Config.yml (https://github.com/LANCommander/LANCommander.Interposer).\n", f);
    fprintf(f,
            "DirectInput:\n"
            "  # the game's DirectInput 7 bridged to DirectInput 8 (modern Windows' dinput.dll overruns its device\n"
            "  # array while it enumerates HID devices: a hang or crash at startup)\n"
            "  FixLegacyDeviceEnumeration: %s\n"
            "  DeviceFilter:\n"
            "    Enabled: %s\n"
            "    Classes: ['Mouse', 'Keyboard']\n",
            dinput ? "true" : "false", kbm ? "true" : "false");
    fprintf(f,
            "FastDL:\n"
            "  # worlds the game lacks, downloaded from the FastDL server of the game server it joins\n"
            "  Enabled: %s\n"
            "  ProbeConnections: false\n"
            "  # into the game folder itself: the engine looks for a world with FindFirstFile there before it opens it\n"
            "  UseDownloadDirectory: false\n"
            "  AllowedExtensions: [.dat, .dtx, .wav, .spr, .abc, .txt]\n"
            "  Paths:\n"
            "    - Local: '%%GAMEDIR%%\\Data\\Worlds'\n"
            "      Remote: Worlds\n",
            fastdl ? "true" : "false");
    discord = write_presence(f);
    fclose(f);
    lm_log("interposer: wrote %s (DirectInput fix %s, FastDL %s, Discord %s)", path, dinput ? "on" : "off",
           fastdl ? "on" : "off", discord ? "on" : "off");
}
