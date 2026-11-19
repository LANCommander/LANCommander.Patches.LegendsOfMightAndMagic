/* Core of the patch: recognises the program, reads the settings, installs the patch modules.
 * Settings and log go to lommpatch.ini / lommpatch.log next to the DLL (dinput.dll proxy and ASI builds), or to the
 * LANCommander Interposer's Config.yml / session log (plugin build, see interposer_plugin.c). The player's own values,
 * console variables in autoexec.cfg in the save folder (settings.c), override either, per key.
 *
 * Supported programs (anything else is left alone and logged):
 * - lithtech.exe of Legends of Might and Magic 1.1 (PE timestamp 0x31313931, entry 0x6fbdd): the game, which also
 *   hosts listen servers;
 * - LegendServ.exe 1.0 (PE timestamp 0x3b4b6bd4, entry 0x10163): the dedicated server, which loads server.dll and
 *   object.lto. Only the server side of the patch (voice server, query replies, FastDL advertising) runs there. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lommpatch.h"

#ifdef LM_INTERPOSER_EMBEDDED
#include "interposer/LANCommander.Interposer.Embedded.h"
#endif

static const struct {
    const char *name;
    DWORD timestamp, entry;
    int process;
} PROGRAMS[] = {
    {"lithtech.exe 1.1", 0x31313931u, 0x6fbddu, LM_PROC_CLIENT},
    {"LegendServ.exe 1.0", 0x3b4b6bd4u, 0x10163u, LM_PROC_DEDICATED},
};

static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];
static int (*g_config_int)(const char *section, const char *key, int def);
static void (*g_log_line)(const char *line);
static int (*g_config_str)(const char *section, const char *key, char *out, int size);
static CRITICAL_SECTION g_log_cs;
static LONG g_log_cs_state;
static HMODULE g_module;
static int g_process = -1;
#ifdef LM_INTERPOSER_EMBEDDED
static int g_embedded;               /* the embedded Interposer was attached */
#endif

void lm_set_backend(int (*config_int)(const char *section, const char *key, int def), void (*log_line)(const char *line))
{
    g_config_int = config_int;
    g_log_line = log_line;
}

void lm_set_string_backend(int (*config_str)(const char *section, const char *key, char *out, int size))
{
    g_config_str = config_str;
}

HMODULE lm_module(void)
{
    return g_module;
}

void lm_set_module(HMODULE dll)
{
    g_module = dll;
}

/* any thread */
void lm_log(const char *fmt, ...)
{
    va_list ap;
    char line[1024];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log_line) {
        g_log_line(line);
        return;
    }
    if (!g_log[0]) {
        return;
    }
    if (InterlockedCompareExchange(&g_log_cs_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_log_cs);
        InterlockedExchange(&g_log_cs_state, 2);
    }
    while (g_log_cs_state != 2) {
        Sleep(0);
    }
    EnterCriticalSection(&g_log_cs);
    {
        FILE *f = fopen(g_log, "a");
        if (f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                    GetCurrentProcessId(), line);
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

/* ---------------------------------------------------------------- memory */

int lm_readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY
                   | PAGE_EXECUTE_WRITECOPY;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & ok)
        || (mbi.Protect & PAGE_GUARD)) {
        return 0;
    }
    return (const BYTE *)p + n <= (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

int lm_write(uintptr_t addr, const void *bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(LM_ADDR(addr), len, PAGE_EXECUTE_READWRITE, &old)) {
        lm_log("VirtualProtect %08lx failed (%lu)", (unsigned long)addr, GetLastError());
        return 0;
    }
    memcpy(LM_ADDR(addr), bytes, len);
    VirtualProtect(LM_ADDR(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), LM_ADDR(addr), len);
    return 1;
}

int lm_expect(uintptr_t addr, const void *bytes, size_t len)
{
    return lm_readable(LM_ADDR(addr), len) && memcmp(LM_ADDR(addr), bytes, len) == 0;
}

int lm_patch(uintptr_t addr, const void *expect, const void *bytes, size_t len)
{
    if (!lm_expect(addr, expect, len)) {
        lm_log("patch %08lx: original bytes differ, skipped", (unsigned long)addr);
        return 0;
    }
    return lm_write(addr, bytes, len);
}

/* op rel32 at from, then NOPs up to len bytes */
static int rel32(uintptr_t from, uint8_t op, const void *to, size_t len)
{
    uint8_t b[32];
    int32_t rel = (int32_t)((uintptr_t)to - (from + 5));
    if (len < 5 || len > sizeof b) {
        return 0;
    }
    b[0] = op;
    memcpy(b + 1, &rel, 4);
    memset(b + 5, 0x90, len - 5);
    return lm_write(from, b, len);
}

int lm_jmp(uintptr_t from, const void *to) { return rel32(from, 0xe9, to, 5); }
int lm_call(uintptr_t from, const void *to) { return rel32(from, 0xe8, to, 5); }

/* room for the trampolines, a page at a time */
static BYTE *trampoline(size_t len)
{
    static BYTE *page;
    static size_t used;
    BYTE *t;
    len = (len + 15) & ~(size_t)15;
    if (!page || used + len > 4096) {
        if (!(page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
            lm_log("VirtualAlloc for a trampoline failed (%lu)", GetLastError());
            return NULL;
        }
        used = 0;
    }
    t = page + used;
    used += len;
    return t;
}

void *lm_detour(uintptr_t fn, const void *prologue, size_t len, const void *hook)
{
    BYTE *t;
    int32_t rel;
    if (len < 5 || !lm_expect(fn, prologue, len) || !(t = trampoline(len + 5))) {
        return NULL;
    }
    memcpy(t, prologue, len);
    t[len] = 0xe9;
    rel = (int32_t)((fn + len) - ((uintptr_t)t + len + 5));
    memcpy(t + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, len + 5);
    return rel32(fn, 0xe9, hook, len) ? t : NULL;
}

int lm_hook_slot(uintptr_t slot, uintptr_t expect, const void *hook)
{
    if (!lm_readable(LM_ADDR(slot), 4) || *(const uintptr_t *)LM_ADDR(slot) != expect) {
        return 0;
    }
    return lm_write(slot, &hook, sizeof hook);
}

int lm_redirect_call(uintptr_t site, uintptr_t old_target, const void *to)
{
    uint8_t old[5];
    int32_t rel = (int32_t)(old_target - (site + 5));
    old[0] = 0xe8;
    memcpy(old + 1, &rel, 4);
    if (!lm_expect(site, old, 5)) {
        lm_log("call at %08lx does not go to %08lx, left alone", (unsigned long)site, (unsigned long)old_target);
        return 0;
    }
    return lm_call(site, to);
}

/* ---------------------------------------------------------------- settings */

const char *lm_ini_file(void)
{
    if (!g_ini[0]) {   /* Interposer plugin: settings the patch writes itself still go to lommpatch.ini */
        snprintf(g_ini, sizeof g_ini, "%slommpatch.ini", lm_game_dir());
    }
    return g_ini;
}

/* the default of [section] key: lommpatch.ini, or the Interposer's Config.yml (plugin build); 1 if it has one */
int lm_ini_default(const char *section, const char *key, char *out, int size)
{
    out[0] = '\0';
    if (g_config_int) {   /* Interposer backend */
        return g_config_str ? g_config_str(section, key, out, size) : 0;
    }
    GetPrivateProfileStringA(section, key, "\x01", out, size, lm_ini_file());
    if (!strcmp(out, "\x01")) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* the player's value (a console variable in autoexec.cfg, settings.c) wins over the default */
int lm_ini_int(const char *section, const char *key, int def)
{
    char value[64], *end;
    long n;
    if (lm_settings_lookup(section, key, value, sizeof value)) {
        n = strtol(value, &end, 10);
        return end == value ? def : (int)n;
    }
    if (g_config_int) {
        return g_config_int(section, key, def);
    }
    return (int)GetPrivateProfileIntA(section, key, def, lm_ini_file());
}

int lm_ini_str(const char *section, const char *key, char *out, int size)
{
    out[0] = '\0';
    if (lm_settings_lookup(section, key, out, size)) {
        return out[0] != '\0';
    }
    lm_ini_default(section, key, out, size);
    return out[0] != '\0';
}

/* ---------------------------------------------------------------- start */

static int identify(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    size_t i;
    if ((uintptr_t)dos != 0x400000) {
        return -1;
    }
    for (i = 0; i < LM_COUNT(PROGRAMS); i++) {
        if (nt->FileHeader.TimeDateStamp == PROGRAMS[i].timestamp
            && nt->OptionalHeader.AddressOfEntryPoint == PROGRAMS[i].entry) {
            return (int)i;
        }
    }
    return -1;
}

int lm_process(void)
{
    if (g_process < 0) {
        int i = identify();
        g_process = i < 0 ? LM_PROC_NONE : PROGRAMS[i].process;
    }
    return g_process;
}

int lm_patch_start(void)
{
    int i = identify();
    if (i < 0) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        g_process = LM_PROC_NONE;
        lm_log("lommpatch: %s is not a supported lithtech.exe 1.1 or LegendServ.exe 1.0; no patches applied", exe);
        return 0;
    }
    g_process = PROGRAMS[i].process;
    if (!lm_ini_int("General", "Enabled", 1)) {
        lm_log("lommpatch: disabled in the settings");
        return 0;
    }
    {
        /* the dinput.dll proxy, the ASI and the Interposer plugin carry the same patches: only the first one loaded
         * applies them */
        char name[48];
        snprintf(name, sizeof name, "Local\\lommpatch_%lu", GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, name) && GetLastError() == ERROR_ALREADY_EXISTS) {
            lm_log("lommpatch: another copy of the patch is already loaded; this one does nothing");
            return 0;
        }
    }
    lm_log("lommpatch " LM_VERSION ": %s recognised, installing (command line: %s)", PROGRAMS[i].name,
           GetCommandLineA());
    return 1;
}

void lm_install(void)
{
    lm_crashlog_install();
    lm_modules_install();
    if (lm_process() == LM_PROC_CLIENT) {
        lm_saves_install();
        lm_registry_install();
        lm_input_install();
        lm_widescreen_install();
        lm_window_install();
        lm_ui_install();
        lm_framerate_install();
        lm_voice_install();
        lm_fastdl_install();
        lm_presence_install();
    }
    lm_voice_server_install();      /* listen servers in the game, and the dedicated server */
}

const char *lm_log_file(void)
{
    return g_log_line ? "" : g_log;
}

void lm_set_files(const char *ini, const char *log)
{
    lstrcpynA(g_ini, ini, MAX_PATH);
    lstrcpynA(g_log, log, MAX_PATH);
}

/* the dinput.dll proxy and the ASI: lommpatch.ini / lommpatch.log next to the DLL, the embedded Interposer, then the
 * patches */
void lm_standalone_init(HINSTANCE dll)
{
    char ini[MAX_PATH], log[MAX_PATH + 16], *slash;
    lm_set_module(dll);
    GetModuleFileNameA(dll, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    strcpy(log, ini);
    strcat(ini, "lommpatch.ini");
    strcat(log, "lommpatch.log");
    {
        /* a fresh log each time the game starts; the dedicated server (in the same folder) adds to it */
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (lm_process() == LM_PROC_CLIENT
            || (GetFileAttributesExA(log, GetFileExInfoStandard, &a) && a.nFileSizeLow > (4u << 20))) {
            DeleteFileA(log);
        }
    }
    lm_set_files(ini, log);
#ifdef LM_INTERPOSER_EMBEDDED
    if (lm_process() != LM_PROC_NONE && lm_ini_int("General", "Enabled", 1)) {
        /* its Config.yml (the DirectInput fix, FastDL) is written before it starts, which reads it then */
        lm_interposer_write_config(dll);
        g_embedded = 1;
        if (InterposerEmbeddedAttach(dll)) {
            lm_log("interposer: the embedded LANCommander Interposer runs this process");
        } else {
            lm_log("interposer: another LANCommander Interposer runs this process; the patch uses that one");
        }
        lm_voice_set_interposer(dll);   /* this image exports the Interposer's functions (forwarded if not the owner) */
    }
#endif
    if (lm_patch_start()) {
        lm_install();
    }
}

void lm_standalone_detach(BOOL process_terminating)
{
#ifdef LM_INTERPOSER_EMBEDDED
    if (g_embedded) {
        InterposerEmbeddedDetach(process_terminating);
    }
#else
    (void)process_terminating;
#endif
}
