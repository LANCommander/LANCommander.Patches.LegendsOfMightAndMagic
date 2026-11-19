/* The voice keys as game controls: "Voice: talk nearby" (action VoiceProximity, code 120, CapsLock by default) and
 * "Voice: team radio" (VoiceTeam, 121, X by default; B is the buy menu's). They are listed in Options > Controls,
 * bound there like any other control, and kept in autoexec.cfg by the engine (AddAction / rangebind lines).
 *
 * - The engine's actions (lithtech.exe: list at 0x493aa8, sentinel 0x493a80; node +0 code, +4 name, +0x28 next) come
 *   from autoexec.cfg, which the engine runs before the client shell's OnEngineInitialized (cshell vtable slot RVA
 *   0x8fe80 -> 0x625ab). There, the actions missing are added with their default keys ("AddAction", "rangebind"
 *   through RunConsoleString) and autoexec.cfg is written (ILTClient WriteConfigFile, +0x68), so the Controls
 *   screen's Cancel, which re-reads the file, keeps them.
 * - The Controls screen lists a fixed table of 36 action codes (RVA 0x9a068) and names (0x9a0f8). The table is copied
 *   with the two voice actions added and the screen's code pointed at the copy (9 operands: the bounds and the
 *   offsets of 0x4a8d2 list model, 0x4aa01 unbind, 0x4aede rows, 0x4b548 save). Their display names come from
 *   GetActionText (0x4b1cb), which the patch answers for the two codes.
 * - Restore defaults (0x4b1a3) runs controldefaults.cfg (ILTClient +0x64) at RVA 0x4b1b2; the patch's stub runs it
 *   and binds the voice keys' defaults after it.
 * - Held or not: ILTClient IsCommandOn (0x40884c, cdecl int(int code)). The engine clears every command while the
 *   console or the chat line is open (SetInputState(0)), while a key is being bound and while the game is not in the
 *   foreground; voice.c also only listens while the game screen is the current one (not in the menus). */
#include <stdio.h>
#include <string.h>
#include "voice.h"

#define THISCALL __attribute__((thiscall))

#define ACTIONS_HEAD      0x493aa8u
#define ACTIONS_END       0x493a80u
#define ENGINE_RUN        0x40887bu  /* cdecl RunConsoleString */
#define IS_COMMAND_ON     0x40884cu  /* cdecl int (int code) */

/* cshell RVAs */
#define CS_CLIENT         0xa42f8u
#define CS_CODES          0x9a068u
#define CS_NAMES          0x9a0f8u
#define CS_GET_TEXT       0x4b1cbu
#define CS_DEFAULTS_RUN   0x4b1b2u
#define CS_CONTROLDEFAULTS 0x9a30cu  /* "controldefaults.cfg" */
#define CS_ON_ENGINE_INIT_SLOT 0x8fe80u
#define CS_ON_ENGINE_INIT 0x625abu
#define CS_GAME_SCREEN    0xa4874u
#define CS_SCREEN_MGR     0xa4cb4u
#define GAME_ACTIONS      36

static const struct {
    const char *action, *text;
    int code, scancode;              /* the default key: DirectInput scan code */
} VOICE[2] = {
    {"VoiceProximity", "Voice: talk nearby", 120, 58},   /* CapsLock */
    {"VoiceTeam", "Voice: team radio", 121, 45},         /* X */
};

static uintptr_t g_cs;
static int g_codes[GAME_ACTIONS + 2];
static const char *g_names[GAME_ACTIONS + 2];
static const char *(__fastcall *g_get_text)(void *self, void *edx, int code);
static int (THISCALL *g_on_engine_init)(void *self, void *a, void *b);
static int g_ok;                     /* IsCommandOn is the expected function */

static void run(const char *cmd)
{
    ((void (__cdecl *)(const char *))LM_ADDR(ENGINE_RUN))(cmd);
}

static void *client(void)
{
    void *c = g_cs ? *(void **)(g_cs + CS_CLIENT) : NULL;
    return c && lm_readable(c, 0x100) ? c : NULL;
}

static void write_config(void)
{
    void *c = client();
    if (c) {
        ((int (__cdecl *)(const char *))(*(void **)((BYTE *)c + 0x68)))("autoexec.cfg");
    }
}

static int action_exists(const char *name)
{
    BYTE *p;
    for (p = *(BYTE **)LM_ADDR(ACTIONS_HEAD); p && p != (BYTE *)LM_ADDR(ACTIONS_END); p = *(BYTE **)(p + 0x28)) {
        if (!lm_readable(p, 0x2c)) {
            break;
        }
        if (!_stricmp((const char *)(p + 4), name)) {
            return 1;
        }
    }
    return 0;
}

static void bind_default(int i)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rangebind \"##keyboard\" \"##%d\" 0 0 \"%s\"", VOICE[i].scancode, VOICE[i].action);
    run(cmd);
}

/* the voice actions the engine does not have yet, with their default keys */
static void register_actions(void)
{
    char cmd[64];
    int i, added = 0;
    for (i = 0; i < 2; i++) {
        if (action_exists(VOICE[i].action)) {
            continue;
        }
        snprintf(cmd, sizeof cmd, "AddAction %s %d", VOICE[i].action, VOICE[i].code);
        run(cmd);
        bind_default(i);
        lm_log("voice keys: added the control %s (scan code %d)", VOICE[i].action, VOICE[i].scancode);
        added++;
    }
    if (added) {
        write_config();
    }
}

static int THISCALL hk_on_engine_init(void *self, void *a, void *b)
{
    register_actions();
    return g_on_engine_init(self, a, b);
}

static const char *__fastcall hk_get_text(void *self, void *edx, int code)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (code == VOICE[i].code) {
            return VOICE[i].text;
        }
    }
    return g_get_text(self, edx, code);
}

/* Restore defaults: controldefaults.cfg, then the voice keys' defaults */
static void __cdecl defaults_stub(const char *file)
{
    void *c = client();
    int i;
    if (c) {
        ((void (__cdecl *)(const char *))(*(void **)((BYTE *)c + 0x64)))(file);
    }
    for (i = 0; i < 2; i++) {
        bind_default(i);
    }
}

int lm_voice_key_held(int which)
{
    void *game, *mgr;
    if (!g_ok || !g_cs || which < 0 || which > 1) {
        return 0;
    }
    game = *(void **)(g_cs + CS_GAME_SCREEN);
    mgr = *(void **)(g_cs + CS_SCREEN_MGR);
    if (!game || !lm_readable(mgr, 8) || *(void **)((BYTE *)mgr + 4) != game) {
        return 0;                    /* a menu is open */
    }
    return ((int (__cdecl *)(int))LM_ADDR(IS_COMMAND_ON))(VOICE[which].code) != 0;
}

/* an absolute operand: written if it holds what it should */
static int operand(uintptr_t at, uint32_t expect, uint32_t value)
{
    if (!lm_readable((void *)at, 4) || *(uint32_t *)at != expect) {
        lm_log("voice keys: cshell %08lx holds %08lx, not %08lx", (unsigned long)(at - g_cs),
               lm_readable((void *)at, 4) ? (unsigned long)*(uint32_t *)at : 0ul, (unsigned long)expect);
        return 0;
    }
    return lm_write(at, &value, 4);
}

static void on_cshell(HMODULE m)
{
    static const BYTE text_head[5] = {0x8b, 0x44, 0x24, 0x04, 0x56};
    static const BYTE is_on_head[10] = {0x8b, 0x4c, 0x24, 0x04, 0x81, 0xf9, 0xff, 0x00, 0x00, 0x00};
    uintptr_t cs = (uintptr_t)m;
    uint32_t codes = (uint32_t)(uintptr_t)g_codes, names = (uint32_t)(uintptr_t)g_names;
    uint32_t old_codes = (uint32_t)(cs + CS_CODES), old_names = (uint32_t)(cs + CS_NAMES);
    BYTE expect[14], repl[14];
    int32_t rel;
    int i, ok;
    g_cs = cs;
    g_ok = lm_expect(IS_COMMAND_ON, is_on_head, sizeof is_on_head);
    if (!g_ok) {
        lm_log("voice keys: the engine's IsCommandOn differs, the voice keys do nothing");
        return;
    }
    /* the actions, at OnEngineInitialized (autoexec.cfg has been run then) */
    g_on_engine_init = (int (THISCALL *)(void *, void *, void *))(cs + CS_ON_ENGINE_INIT);
    if (!lm_hook_slot(cs + CS_ON_ENGINE_INIT_SLOT, cs + CS_ON_ENGINE_INIT, (const void *)hk_on_engine_init)) {
        lm_log("voice keys: cshell's OnEngineInitialized differs, the voice controls are not added");
        return;
    }
    /* the Controls screen's table, with the voice actions */
    if (!lm_readable((void *)(cs + CS_CODES), GAME_ACTIONS * 8)) {
        return;
    }
    memcpy(g_codes, (void *)(cs + CS_CODES), GAME_ACTIONS * 4);
    memcpy(g_names, (void *)(cs + CS_NAMES), GAME_ACTIONS * 4);
    for (i = 0; i < 2; i++) {
        g_codes[GAME_ACTIONS + i] = VOICE[i].code;
        g_names[GAME_ACTIONS + i] = VOICE[i].action;
    }
    ok = operand(cs + 0x4a91d, old_codes, codes)
         && operand(cs + 0x4a937, old_codes + GAME_ACTIONS * 4, codes + (GAME_ACTIONS + 2) * 4)
         && operand(cs + 0x4aa0d, old_codes, codes)
         && operand(cs + 0x4aa75, old_codes + GAME_ACTIONS * 4, codes + (GAME_ACTIONS + 2) * 4)
         && operand(cs + 0x4af2d, old_codes, codes)
         && operand(cs + 0x4b194, old_codes + GAME_ACTIONS * 4, codes + (GAME_ACTIONS + 2) * 4)
         && operand(cs + 0x4b562, old_codes, codes)
         && operand(cs + 0x4b585, old_names, names)
         && operand(cs + 0x4b5d3, GAME_ACTIONS * 4, (GAME_ACTIONS + 2) * 4);
    if (!ok) {
        lm_log("voice keys: the Controls screen differs (some of it may point at the new table); the voice keys are "
               "bound in autoexec.cfg only");
        return;
    }
    g_get_text = (const char *(__fastcall *)(void *, void *, int))lm_detour(cs + CS_GET_TEXT, text_head,
                                                                           sizeof text_head, (const void *)hk_get_text);
    /* Restore defaults: mov eax,[client]; push "controldefaults.cfg"; call [eax+0x64]; pop ecx */
    expect[0] = 0xa1;
    *(uint32_t *)(expect + 1) = (uint32_t)(cs + CS_CLIENT);
    expect[5] = 0x68;
    *(uint32_t *)(expect + 6) = (uint32_t)(cs + CS_CONTROLDEFAULTS);
    memcpy(expect + 10, "\xff\x50\x64\x59", 4);
    repl[0] = 0x68;
    *(uint32_t *)(repl + 1) = (uint32_t)(cs + CS_CONTROLDEFAULTS);
    repl[5] = 0xe8;
    rel = (int32_t)((uintptr_t)defaults_stub - (cs + CS_DEFAULTS_RUN + 10));
    memcpy(repl + 6, &rel, 4);
    memcpy(repl + 10, "\x59\x90\x90\x90", 4);
    if (!lm_patch(cs + CS_DEFAULTS_RUN, expect, repl, sizeof repl)) {
        lm_log("voice keys: Restore defaults differs, it leaves the voice keys unbound");
    }
    lm_log("voice keys: in Options > Controls (%s)", g_get_text ? "named" : "names missing");
}

static void on_cshell_unload(void)
{
    g_cs = 0;
}

void lm_voice_keys_install(void)
{
    lm_on_module(LM_MOD_CSHELL, on_cshell);
    lm_on_module_unload(LM_MOD_CSHELL, on_cshell_unload);
}
