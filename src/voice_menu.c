/* The voice chat settings in the game's options: Options > Sound > "Voice chat..." opens a Voice panel with the voice
 * volume, the microphone volume, the mode (push to talk or voice activated) and the microphone. The keys are ordinary
 * controls in the Controls screen (voice_keys.c).
 *
 * cshell.dll (RVAs; OptionsMenus.cpp): the Sound panel is a screen of 0x1a0 bytes (ctor 0x50a25, vtable 0x8e238, 71
 * slots) built by 0x50aa0 (called from 0x50a99 only) on the background "sound options.pcx" drawn at design (0xe1, 0).
 * Its controls report clicks to the screen's OnCommand (vtable +0x5c, 0x51150) with their id (+0xc). The panel is
 * built anew each time it opens and deleted when it closes (0x4c280).
 * - 0x50a99's call goes through sound_build, which adds a "Voice chat..." row (id 0x40) in the free space under the
 *   volume slider.
 * - The Voice panel's background is its own copy of the Sound panel's picture with dist/voice options.png (built into
 *   the DLL) written over it: the same picture with a second slider track, for the microphone's volume, and no EAX
 *   logo. Should that not decode, the logo and the title are covered with stone from the same picture.
 * - The Sound panel's OnCommand slot goes through sound_cmd: id 0x40 opens the Voice panel, a second Sound panel object
 *   (same ctor and background) whose vtable is a copy of the Sound panel's with OnCommand replaced by voice_cmd, filled
 *   with the game's own control classes: clickable text (0x84 bytes, ctor 0x49db4), slider (0x70, ctor 0x34592; the
 *   track is baked into the background at the first row), image buttons (0x33938). It is shown with the options
 *   screen's own panel switch (0x4c24f), which also deletes the Sound panel.
 * - OK writes the settings (console variables, settings.c) and has the engine write autoexec.cfg (ILTClient
 *   WriteConfigFile, +0x68), then goes back to the options screen (0x4c280); Cancel goes back without them. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice.h"
#include <wincodec.h>

#define THISCALL __attribute__((thiscall))

/* cshell RVAs */
#define CS_NEW            0x7d44u    /* cdecl void *(size_t, const char *file, int line) */
#define CS_DRAW_MGR       0xa4ca4u
#define CS_FONT_MGR       0xa4008u
#define CS_FONT_GET       0x65b03u   /* thiscall(font mgr, index) */
#define CS_FONT_COLOR     0x658b3u   /* thiscall(font mgr, index) */
#define CS_CLIENT         0xa42f8u   /* ILTClient * */
#define CS_TEXT_CTOR      0x49db4u   /* thiscall(this, 0x1c, draw mgr, id) */
#define CS_SLIDER_CTOR    0x34592u   /* thiscall(this, draw mgr, -1) */
#define CS_SURFACE        0x65d42u   /* cdecl(pcx, 1, -1, 1.0f) */
#define CS_IMAGE_BUTTON   0x33938u   /* cdecl(normal, down, high, 0, x, y, id, 1) */
#define CS_SOUND_CTOR     0x50a25u   /* thiscall(this, options screen) */
#define CS_SOUND_BUILD    0x50aa0u
#define CS_SOUND_BUILD_CALL 0x50a99u
#define CS_SOUND_VTBL     0x8e238u
#define CS_SOUND_CMD      0x51150u
#define CS_PANEL_SHOW     0x4c24fu   /* thiscall(options screen, panel) */
#define CS_PANEL_CLOSE    0x4c280u   /* thiscall(options screen, panel) */
#define SOUND_VTBL_SLOTS  71
#define SOUND_PARENT      0x18c
#define SOUND_SIZE        0x1a0
/* the art of the game's buttons: normal, down, highlighted */
#define CS_ART_OK         0x9a77cu, 0x9a7a4u, 0x9a7c8u
#define CS_ART_CANCEL     0x9a6fcu, 0x9a728u, 0x9a750u
#define CS_ART_DEFAULT    0x9a7f0u, 0x9a81cu, 0x9a848u
#define CS_ART_LEFT       0x9a940u, 0x9a970u, 0x9a99cu
#define CS_ART_RIGHT      0x9a8acu, 0x9a8dcu, 0x9a90cu

/* control vtable slots */
#define V_CMD             0x5c
#define V_HIDE            0xa4
#define V_RELATIVE        0x9c
#define V_ADD_CHILD       0xd0       /* of a screen */
#define V_SET_BG          0x110      /* of a screen */
#define V_TEXT_INIT       0xd0       /* of a text control */
#define V_TEXT_SET        0xd4
#define V_HOTKEY          0x50
#define V_SLIDER_INIT     0xf4
#define V_SLIDER_GET      0x108
#define V_SLIDER_SET      0x10c

enum {
    ID_OK = 0, ID_CANCEL = 1, ID_DEFAULT = 2, ID_VOL_DOWN = 3, ID_VOL_UP = 4, ID_MIC_DOWN = 8, ID_MIC_UP = 9,
    ID_MODE = 10, ID_DEVICE = 11, ID_VOICE_ROW = 0x40, ID_LABEL = -1
};

#define VF(obj, off) ((*(void ***)(obj))[(off) / 4])

static uintptr_t g_cs;
static void *g_voice_vtbl[SOUND_VTBL_SLOTS];
static char (THISCALL *g_sound_cmd)(void *self, void *ctrl);

/* the Voice panel that is open (only ever one), and its state */
static struct {
    void *panel, *slider, *mic_slider, *mic_text, *mode_text, *device_text;
    int volume, mic_volume, mode, device, mic_track;
    int n;
    lm_voice_device devices[16];
} g_v;

static void *cs_new(size_t size)
{
    return ((void *(__cdecl *)(size_t, const char *, int))(g_cs + CS_NEW))(size, "voice_menu.c", 0);
}

static void *font(int index)
{
    return ((void *(THISCALL *)(void *, int))(g_cs + CS_FONT_GET))((void *)(g_cs + CS_FONT_MGR), index);
}

static DWORD color(void)
{
    return ((DWORD (THISCALL *)(void *, int))(g_cs + CS_FONT_COLOR))((void *)(g_cs + CS_FONT_MGR), 0);
}

static void add(void *screen, void *ctrl)
{
    ((void (THISCALL *)(void *, void *))VF(screen, V_ADD_CHILD))(screen, ctrl);
}

/* the game's clickable text: relative = placed in the panel (its labels), else in design coordinates */
static void *text_font(void *screen, int id, int x, int y, int w, int h, const char *s, int relative, int hover,
                       int font_index)
{
    void *t = cs_new(0x84), *f = font(font_index);
    if (!t) {
        return NULL;
    }
    ((void *(THISCALL *)(void *, int, void *, int))(g_cs + CS_TEXT_CTOR))(t, 0x1c, *(void **)(g_cs + CS_DRAW_MGR), id);
    ((void (THISCALL *)(void *, int, int, int, int, void *, DWORD, int, int))VF(t, V_TEXT_INIT))(t, x, y, w, h, f,
                                                                                               color(), -1, 0);
    if (relative) {
        ((void (THISCALL *)(void *, int))VF(t, V_RELATIVE))(t, 1);
    }
    *((BYTE *)t + 0x74) = 0;
    ((void (THISCALL *)(void *, const char *, int))VF(t, V_TEXT_SET))(t, s, 0);
    *(void **)((BYTE *)t + 0x7c) = hover ? font(0x27) : f;   /* the yellow font under the mouse */
    *(void **)((BYTE *)t + 0x80) = *(void **)((BYTE *)t + 0x48);
    add(screen, t);
    return t;
}

static void *text(void *screen, int id, int x, int y, int w, int h, const char *s, int relative, int hover)
{
    return text_font(screen, id, x, y, w, h, s, relative, hover, 0x1c);   /* fonts\norse font 10pt.pcx */
}

/* ---------------------------------------------------------------- the background */

/* dist/voice options.png (the Sound panel's picture with a second slider track and no EAX logo), built into the DLL
 * (build.sh: voice_bg.c), decoded once with Windows' own image codecs: 24-bit BGR rows */
extern const unsigned char lm_voice_bg_png[];
extern const unsigned lm_voice_bg_png_size;
static BYTE *g_png;
static int g_png_w, g_png_h;

static int load_png(void)
{
    static const GUID clsid_factory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    static const GUID iid_factory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
    static const GUID bgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
    static int tried;
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    HRESULT co;
    if (tried) {
        return g_png != NULL;
    }
    tried = 1;
    co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(CoCreateInstance(&clsid_factory, NULL, CLSCTX_INPROC_SERVER, &iid_factory, (void **)&factory))
        && SUCCEEDED(IWICImagingFactory_CreateStream(factory, &stream))
        && SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)lm_voice_bg_png, lm_voice_bg_png_size))
        && SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL,
                                                                WICDecodeMetadataCacheOnDemand, &decoder))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame))
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &conv))
        && SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &bgr24, WICBitmapDitherTypeNone,
                                                    NULL, 0.0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h)) && w && h && w <= 2048 && h <= 2048
        && (g_png = malloc((size_t)w * h * 3)) != NULL) {
        if (SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, w * 3, w * h * 3, g_png))) {
            g_png_w = (int)w;
            g_png_h = (int)h;
        } else {
            free(g_png);
            g_png = NULL;
        }
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (co == S_OK || co == S_FALSE) {
        CoUninitialize();
    }
    if (g_png) {
        lm_log("voice menu: its background picture, %dx%d", g_png_w, g_png_h);
    } else {
        lm_log("voice menu: cannot decode its background picture; the Sound panel's is retouched instead");
    }
    return g_png != NULL;
}

/* the surface's pixel format, found by writing pure red, green and blue with the engine's SetPixel and reading the
 * bytes back (16-bit 565 or 555, or 32-bit) */
static int pixel_masks(void *surf, int bpp, DWORD masks[3])
{
    static const DWORD colours[3] = {0xff0000, 0x00ff00, 0x0000ff};
    int i, pitch;
    BYTE *bits;
    for (i = 0; i < 3; i++) {
        ((int (__cdecl *)(void *, int, int, DWORD))LM_ADDR(0x40a592u))(surf, 0, 0, colours[i]);   /* SetPixel */
        if (!(bits = ((BYTE *(__cdecl *)(void *, int *, int))LM_ADDR(0x40a34du))(surf, &pitch, 0))) {
            return 0;
        }
        masks[i] = 0;
        memcpy(&masks[i], bits, (size_t)bpp);
        ((void (__cdecl *)(void *))LM_ADDR(0x40a3c3u))(surf);
        if (!masks[i]) {
            return 0;
        }
    }
    return 1;
}

/* an 8-bit channel into the bits of mask */
static DWORD channel(BYTE v, DWORD mask)
{
    int shift = 0, bits = 0;
    while (!(mask >> shift & 1)) shift++;
    while (mask >> (shift + bits) & 1) bits++;
    return ((DWORD)v >> (8 - (bits > 8 ? 8 : bits))) << shift & mask;
}

/* the Voice panel's background, its own copy of the Sound panel's (every screen loads its own), replaced with
 * lommpatch\voice options.png; 1 if done */
static int replace_background(void *screen)
{
    void *surf = *(void **)((BYTE *)screen + 0x70);
    int bpp = *(int *)LM_ADDR(0x491004u), pitch = 0, x, y;
    DWORD masks[3];
    BYTE *bits;
    if (!load_png() || !surf || !lm_readable(surf, 0x20) || (bpp != 2 && bpp != 4)
        || *(int *)((BYTE *)surf + 0x10) != g_png_w || *(int *)((BYTE *)surf + 0x14) != g_png_h
        || !pixel_masks(surf, bpp, masks)) {
        return 0;
    }
    /* lock (marks it changed: the engine uploads it again before it is drawn), convert, unlock */
    if (!(bits = ((BYTE *(__cdecl *)(void *, int *, int))LM_ADDR(0x40a34du))(surf, &pitch, 1)) || pitch <= 0) {
        return 0;
    }
    for (y = 0; y < g_png_h; y++) {
        const BYTE *src = g_png + (size_t)y * g_png_w * 3;
        BYTE *dst = bits + (size_t)y * pitch;
        for (x = 0; x < g_png_w; x++, src += 3, dst += bpp) {
            DWORD p = channel(src[2], masks[0]) | channel(src[1], masks[1]) | channel(src[0], masks[2]);
            memcpy(dst, &p, (size_t)bpp);
        }
    }
    ((void (__cdecl *)(void *))LM_ADDR(0x40a3c3u))(surf);
    return 1;
}

/* without the picture: the Sound panel's background without the EAX logo and the title, covered with the plain stone
 * of the same picture */
static void retouch_background(void *screen)
{
    static const struct { int x, y, w, h, from_y; } PATCHES[] = {
        {56, 236, 106, 36, 168},     /* the EAX logo, from the empty box above it */
        {98, 54, 216, 40, 342},      /* "SOUND OPTIONS", from the stone under the box */
    };
    void *surf = *(void **)((BYTE *)screen + 0x70);
    int bpp = *(int *)LM_ADDR(0x491004u), pitch = 0, i, row;
    BYTE *bits;
    if (!surf || !lm_readable(surf, 0x20) || bpp < 1 || bpp > 4
        || *(int *)((BYTE *)surf + 0x10) < 330 || *(int *)((BYTE *)surf + 0x14) < 400) {
        return;
    }
    /* lock (marks it changed: the engine uploads it again before it is drawn), copy, unlock */
    if (!(bits = ((BYTE *(__cdecl *)(void *, int *, int))LM_ADDR(0x40a34du))(surf, &pitch, 1)) || pitch <= 0) {
        return;
    }
    for (i = 0; i < (int)LM_COUNT(PATCHES); i++) {
        for (row = 0; row < PATCHES[i].h; row++) {
            memcpy(bits + (PATCHES[i].y + row) * pitch + PATCHES[i].x * bpp,
                   bits + (PATCHES[i].from_y + row) * pitch + PATCHES[i].x * bpp, (size_t)(PATCHES[i].w * bpp));
        }
    }
    ((void (__cdecl *)(void *))LM_ADDR(0x40a3c3u))(surf);
}

static void set_text(void *t, const char *s)
{
    if (t) {
        ((void (THISCALL *)(void *, const char *, int))VF(t, V_TEXT_SET))(t, s, 0);
    }
}

static void *button(void *screen, uintptr_t normal, uintptr_t down, uintptr_t high, int x, int y, int id, int key)
{
    void *b = ((void *(__cdecl *)(const char *, const char *, const char *, int, int, int, int, int))(
        g_cs + CS_IMAGE_BUTTON))((const char *)(g_cs + normal), (const char *)(g_cs + down),
                                 (const char *)(g_cs + high), 0, x, y, id, 1);
    if (b) {
        if (key) {
            DWORD hotkey = (DWORD)key | 0x100;
            ((void (THISCALL *)(void *, DWORD *))VF(b, V_HOTKEY))(b, &hotkey);
        }
        add(screen, b);
    }
    return b;
}

#define BUTTON(screen, art, x, y, id, key) button_art(screen, (uintptr_t[]){art}, x, y, id, key)
static void *button_art(void *screen, const uintptr_t *art, int x, int y, int id, int key)
{
    return button(screen, art[0], art[1], art[2], x, y, id, key);
}

static void *slider(void *screen, int x, int y, int w, int h, float lo, float hi, float step, float value)
{
    int rect[4] = {x, y, w, h};
    void *s = cs_new(0x70), *knob;
    if (!s) {
        return NULL;
    }
    ((void *(THISCALL *)(void *, void *, int))(g_cs + CS_SLIDER_CTOR))(s, *(void **)(g_cs + CS_DRAW_MGR), -1);
    knob = ((void *(__cdecl *)(const char *, int, int, float))(g_cs + CS_SURFACE))(
        "InterfaceScreens\\Common\\red highlite for slider.pcx", 1, -1, 1.0f);
    ((void (THISCALL *)(void *, int *, void *, float, float, float))VF(s, V_SLIDER_INIT))(s, rect, knob, lo, hi, step);
    ((void (THISCALL *)(void *, float))VF(s, V_SLIDER_SET))(s, value);
    add(screen, s);
    return s;
}

static float slider_value(void *s)
{
    return s ? ((float (THISCALL *)(void *))VF(s, V_SLIDER_GET))(s) : 0.0f;
}

/* ---------------------------------------------------------------- the Voice panel */

/* the bitmap font has ASCII 0x21..0x7e only */
static void ascii(const char *in, char *out, int size, int max_chars)
{
    int n = 0;
    for (; *in && n + 1 < size && n < max_chars; in++) {
        out[n++] = (*in >= 0x20 && *in <= 0x7e) ? *in : '?';
    }
    out[n] = '\0';
}

static void show_values(void)
{
    char s[64];
    snprintf(s, sizeof s, "%d%%", g_v.mic_volume);
    set_text(g_v.mic_text, s);
    set_text(g_v.mode_text, g_v.mode ? "Voice activated" : "Push to talk");
    if (g_v.device <= 0 || g_v.device > g_v.n) {
        set_text(g_v.device_text, "Windows default");
    } else {
        ascii(g_v.devices[g_v.device - 1].name, s, sizeof s, 30);
        set_text(g_v.device_text, s);
    }
    if (g_v.slider) {
        ((void (THISCALL *)(void *, float))VF(g_v.slider, V_SLIDER_SET))(g_v.slider, (float)g_v.volume);
    }
    if (g_v.mic_slider) {
        ((void (THISCALL *)(void *, float))VF(g_v.mic_slider, V_SLIDER_SET))(g_v.mic_slider, (float)g_v.mic_volume);
    }
}

/* the sliders as the mouse left them */
static void read_sliders(void)
{
    if (g_v.slider) {
        g_v.volume = (int)(slider_value(g_v.slider) + 0.5f);
    }
    if (g_v.mic_slider) {
        g_v.mic_volume = (int)(slider_value(g_v.mic_slider) + 0.5f);
    }
}

static void load_values(void)
{
    char mic[256];
    int i;
    g_v.volume = lm_ini_int("Voice", "Volume", 100);
    g_v.volume = g_v.volume < 0 ? 0 : g_v.volume > 100 ? 100 : g_v.volume;
    g_v.mic_volume = lm_ini_int("Voice", "MicVolume", 100);
    g_v.mode = lm_ini_int("Voice", "VoiceActivation", 0) != 0;
    g_v.n = lm_voice_microphones(g_v.devices, (int)LM_COUNT(g_v.devices));
    if (g_v.n > (int)LM_COUNT(g_v.devices)) {
        g_v.n = (int)LM_COUNT(g_v.devices);
    }
    lm_ini_str("Voice", "Microphone", mic, sizeof mic);
    g_v.device = 0;
    for (i = 0; mic[0] && i < g_v.n; i++) {
        if (!strcmp(mic, g_v.devices[i].id)) {
            g_v.device = i + 1;
        }
    }
}

static void save_values(void)
{
    void *client = *(void **)(g_cs + CS_CLIENT);
    read_sliders();
    lm_setting_set_int("VoiceVolume", g_v.volume);
    lm_setting_set_int("VoiceMicVolume", g_v.mic_volume);
    lm_setting_set_int("VoiceActivation", g_v.mode);
    lm_setting_set("VoiceMicrophone", g_v.device > 0 && g_v.device <= g_v.n ? g_v.devices[g_v.device - 1].id : "");
    if (client && lm_readable(client, 0x6c)) {
        ((int (__cdecl *)(const char *))(*(void **)((BYTE *)client + 0x68)))("autoexec.cfg");   /* WriteConfigFile */
    }
    lm_log("voice menu: volume %d, microphone volume %d, %s, microphone %s", g_v.volume, g_v.mic_volume,
           g_v.mode ? "voice activated" : "push to talk",
           g_v.device > 0 && g_v.device <= g_v.n ? g_v.devices[g_v.device - 1].name : "(the default)");
}

static char THISCALL voice_cmd(void *self, void *ctrl)
{
    int id = ctrl ? *(int *)((BYTE *)ctrl + 0xc) : ID_LABEL;
    void *parent = *(void **)((BYTE *)self + SOUND_PARENT);
    if (self != g_v.panel) {
        return 1;                    /* not the panel these controls belong to (cannot happen: one at a time) */
    }
    if (id != ID_OK && id != ID_CANCEL && id != ID_DEFAULT) {
        read_sliders();
    }
    switch (id) {
    case ID_OK:
        save_values();
        /* fall through */
    case ID_CANCEL:
        g_v.panel = NULL;
        ((void (THISCALL *)(void *, void *))(g_cs + CS_PANEL_CLOSE))(parent, self);   /* deletes self: done here */
        return 1;
    case ID_DEFAULT:
        g_v.volume = 100;
        g_v.mic_volume = 100;
        g_v.mode = 0;
        g_v.device = 0;
        break;
    case ID_VOL_DOWN:
    case ID_VOL_UP:
        g_v.volume += id == ID_VOL_UP ? 5 : -5;
        g_v.volume = g_v.volume < 0 ? 0 : g_v.volume > 100 ? 100 : g_v.volume;
        break;
    case ID_MIC_DOWN:
    case ID_MIC_UP:
        g_v.mic_volume += id == ID_MIC_UP ? 10 : -10;
        g_v.mic_volume = g_v.mic_volume < 0 ? 0 : g_v.mic_volume > 200 ? 200 : g_v.mic_volume;
        break;
    case ID_MODE:
        g_v.mode = !g_v.mode;
        break;
    case ID_DEVICE:
        g_v.device = (g_v.device + 1) % (g_v.n + 1);
        break;
    default:
        return 1;
    }
    show_values();
    return 1;
}

/* the Voice panel, in place of the Sound panel (self) */
static void open_voice(void *self)
{
    void *parent = *(void **)((BYTE *)self + SOUND_PARENT), *v;
    if (!parent || !(v = cs_new(SOUND_SIZE))) {
        return;
    }
    ((void *(THISCALL *)(void *, void *))(g_cs + CS_SOUND_CTOR))(v, parent);
    *(void ***)v = g_voice_vtbl;
    ((void (THISCALL *)(void *))VF(v, V_HIDE))(v);
    ((void (THISCALL *)(void *, int, int, const char *))VF(v, V_SET_BG))(
        v, 0xe1, 0, "interfacescreens\\optionsscreen\\sound options.pcx");
    memset(&g_v, 0, sizeof g_v);
    load_values();
    if (replace_background(v)) {
        g_v.mic_track = 1;           /* the picture has a second slider track, 36 px under the first */
    } else {
        retouch_background(v);
        text_font(v, ID_LABEL, 0x16e, 0x3e, 0x90, 0x18, "Voice chat", 0, 0, 0x1d);   /* norse font 12pt */
    }
    /* the voice volume on the slider track of the first row */
    text(v, ID_LABEL, 0x3f, 0x81, 0x89, 0x13, "Voice volume", 1, 0);
    BUTTON(v, CS_ART_LEFT, 0x1b2, 0x81, ID_VOL_DOWN, 0);
    BUTTON(v, CS_ART_RIGHT, 0x227, 0x81, ID_VOL_UP, 0);
    g_v.slider = slider(v, 0x1c3, 0x82, 100, 16, 0.0f, 100.0f, 0.05f, (float)g_v.volume);
    /* the microphone's volume: a slider on the second track, else a percentage between the arrows */
    text(v, ID_LABEL, 0x3f, 0xa5, 0x89, 0x13, "Mic volume", 1, 0);
    BUTTON(v, CS_ART_LEFT, 0x1b2, 0xa5, ID_MIC_DOWN, 0);
    BUTTON(v, CS_ART_RIGHT, 0x227, 0xa5, ID_MIC_UP, 0);
    if (g_v.mic_track) {
        g_v.mic_slider = slider(v, 0x1c3, 0xa6, 100, 16, 0.0f, 200.0f, 0.05f, (float)g_v.mic_volume);
    } else {
        g_v.mic_text = text(v, ID_LABEL, 0x1d8, 0xa6, 0x40, 0x13, "", 0, 0);
    }
    /* the mode: click to change */
    text(v, ID_MODE, 0x3f, 0xcb, 0x89, 0x13, "Voice mode", 1, 0);
    g_v.mode_text = text(v, ID_MODE, 0x3f, 0xe1, 0x12c, 0x13, "", 1, 1);   /* under its label, as the microphone */
    /* the microphone: click for the next one */
    text(v, ID_DEVICE, 0x3f, 0x11b, 0x89, 0x13, "Microphone", 1, 0);
    g_v.device_text = text(v, ID_DEVICE, 0x3f, 0x131, 0x12c, 0x13, "", 1, 1);
    BUTTON(v, CS_ART_DEFAULT, 0x119, 0x185, ID_DEFAULT, 0);
    BUTTON(v, CS_ART_OK, 0x18a, 0x184, ID_OK, 0x0d);
    BUTTON(v, CS_ART_CANCEL, 0x1f9, 0x184, ID_CANCEL, 0x1b);
    show_values();
    g_v.panel = v;
    add(parent, v);
    ((void (THISCALL *)(void *, void *))(g_cs + CS_PANEL_SHOW))(parent, v);   /* shows v, deletes the Sound panel */
}

/* ---------------------------------------------------------------- the Sound panel */

static char THISCALL sound_cmd(void *self, void *ctrl)
{
    if (ctrl && *(int *)((BYTE *)ctrl + 0xc) == ID_VOICE_ROW) {
        open_voice(self);
        return 1;
    }
    return g_sound_cmd(self, ctrl);
}

static int __fastcall sound_build(void *screen)
{
    int r = ((int (THISCALL *)(void *))(g_cs + CS_SOUND_BUILD))(screen);
    text(screen, ID_VOICE_ROW, 0x3f, 0xa6, 0xc8, 0x13, "Voice chat...", 1, 1);
    return r;
}

static void on_cshell(HMODULE m)
{
    static const BYTE cmd_head[7] = {0x56, 0x8b, 0xf1, 0x8b, 0x4c, 0x24, 0x08};
    uintptr_t cs = (uintptr_t)m, slot = cs + CS_SOUND_VTBL + V_CMD;
    g_cs = cs;
    memset(&g_v, 0, sizeof g_v);
    if (!lm_expect(cs + CS_SOUND_CMD, cmd_head, sizeof cmd_head) || !lm_readable((void *)(cs + CS_SOUND_VTBL),
                                                                                 sizeof g_voice_vtbl)
        || *(uintptr_t *)slot != cs + CS_SOUND_CMD) {
        lm_log("voice menu: cshell's Sound options differ, no voice settings in the menus");
        return;
    }
    memcpy(g_voice_vtbl, (void *)(cs + CS_SOUND_VTBL), sizeof g_voice_vtbl);
    g_voice_vtbl[V_CMD / 4] = (void *)voice_cmd;
    g_sound_cmd = (char (THISCALL *)(void *, void *))(cs + CS_SOUND_CMD);
    if (!lm_redirect_call(cs + CS_SOUND_BUILD_CALL, cs + CS_SOUND_BUILD, (const void *)sound_build)) {
        lm_log("voice menu: the Sound panel's build differs, no voice settings in the menus");
        return;
    }
    lm_hook_slot(slot, cs + CS_SOUND_CMD, (const void *)sound_cmd);
    lm_log("voice menu: Options > Sound > Voice chat");
}

static void on_cshell_unload(void)
{
    g_cs = 0;
    memset(&g_v, 0, sizeof g_v);
}

void lm_voice_menu_install(void)
{
    lm_on_module(LM_MOD_CSHELL, on_cshell);
    lm_on_module_unload(LM_MOD_CSHELL, on_cshell_unload);
}
