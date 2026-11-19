/* DirectInput 7 device types for the engine's device list (lithtech.exe), whatever DirectInput really answers.
 *
 * The engine keeps one record per device EnumDevices reports (the callback at 0x42c0a7 copies the instance GUID, the
 * name and dwDevType), and finds "##keyboard", "##mouse" and "Joystick 1" by the low byte of that dwDevType: 3, 2 and 4,
 * the DirectInput 7 DIDEVTYPE_* (0x42c498 / 0x42c51d). The LANCommander Interposer's DirectInput fix
 * (DirectInput.FixLegacyDeviceEnumeration) serves the game a DirectInput 8 object, whose enumeration reports the
 * DirectInput 8 types (DI8DEVTYPE_KEYBOARD 0x13, _MOUSE 0x12, ...). The engine then finds no keyboard and no mouse:
 * every rangebind of autoexec.cfg fails, no key can be bound in the controls menu, and on quitting the game writes an
 * autoexec.cfg without any bindings. The callback is handed to EnumDevices by a push at 0x42c095; it now goes through
 * a wrapper that turns a DirectInput 8 type back into the DirectInput 7 one before the engine sees it.
 * Devices that are no keyboard, mouse or game controller (receivers, LED controllers, consumer control and vendor
 * interfaces of keyboards: DI8DEVTYPE_DEVICE and the like) are not passed on at all. The engine can only bind keyboards,
 * mice and joysticks, and these HID devices, with hundreds of objects at times, did not exist for it in 2001; before
 * this patch their DirectInput 8 types matched nothing in it either. */
#include <string.h>
#include "lommpatch.h"

#define ENUM_PUSH   0x42c095         /* push 0x42c0a7 (the callback), in the engine's input init (0x42c041) */
#define ENUM_CB     0x42c0a7         /* BOOL WINAPI callback(const DIDEVICEINSTANCEA *, void *) */
#define DEVTYPE_OFS 0x24             /* DIDEVICEINSTANCEA::dwDevType */
#define INSTANCE_SIZE 580            /* sizeof(DIDEVICEINSTANCEA), DirectInput 5 and later */

typedef BOOL (WINAPI *enum_cb)(const void *instance, void *ref);

/* a DirectInput 8 dwDevType as DirectInput 7 would have reported it; anything else as it is */
static DWORD dx7_type(DWORD t)
{
    DWORD type = t & 0xff, sub = (t >> 8) & 0xff, flags = t & 0xffff0000u;   /* flags: DIDEVTYPE_HID */
    switch (type) {
    case 0x12:                       /* DI8DEVTYPE_MOUSE: the subtypes agree, but for _ABSOLUTE (6) */
        return flags | (sub > 5 ? 1 : sub) << 8 | 2;
    case 0x13:                       /* DI8DEVTYPE_KEYBOARD: the subtypes agree */
        return flags | sub << 8 | 3;
    case 0x14:                       /* DI8DEVTYPE_JOYSTICK -> DIDEVTYPEJOYSTICK_TRADITIONAL */
        return flags | 2 << 8 | 4;
    case 0x15:                       /* DI8DEVTYPE_GAMEPAD -> DIDEVTYPEJOYSTICK_GAMEPAD */
        return flags | 4 << 8 | 4;
    case 0x16:                       /* DI8DEVTYPE_DRIVING -> DIDEVTYPEJOYSTICK_WHEEL */
        return flags | 6 << 8 | 4;
    case 0x17:                       /* DI8DEVTYPE_FLIGHT -> DIDEVTYPEJOYSTICK_FLIGHTSTICK */
        return flags | 3 << 8 | 4;
    case 0x18:                       /* DI8DEVTYPE_1STPERSON -> DIDEVTYPEJOYSTICK_UNKNOWN */
        return flags | 1 << 8 | 4;
    case 0x11: case 0x19: case 0x1a: case 0x1b: case 0x1c:   /* the rest: not shown to the engine (enum_device) */
        return 0;
    default:
        return t;
    }
}

static BOOL WINAPI enum_device(const BYTE *instance, void *ref)
{
    BYTE copy[INSTANCE_SIZE];
    DWORD size, type, fixed;
    if (!instance) {
        return ((enum_cb)ENUM_CB)(instance, ref);
    }
    size = *(const DWORD *)instance;
    memcpy(&type, instance + DEVTYPE_OFS, sizeof type);
    fixed = dx7_type(type);
    if (fixed == type || size < DEVTYPE_OFS + 4 || size > sizeof copy) {
        return ((enum_cb)ENUM_CB)(instance, ref);
    }
    if (!fixed) {
        lm_log("input: %.64s (type %08lx) is not a keyboard, mouse or game controller: hidden from the engine",
               (const char *)instance + 0x28, (unsigned long)type);
        return TRUE;                 /* DIENUM_CONTINUE */
    }
    memcpy(copy, instance, size);
    memcpy(copy + DEVTYPE_OFS, &fixed, sizeof fixed);
    lm_log("input: %.64s reported as a DirectInput 8 device (type %08lx), the engine sees type %08lx",
           (const char *)instance + 0x28, (unsigned long)type, (unsigned long)fixed);
    return ((enum_cb)ENUM_CB)(copy, ref);
}

void lm_input_install(void)
{
    static const BYTE expect[] = {0x68, 0xa7, 0xc0, 0x42, 0x00};   /* push 0x42c0a7 */
    BYTE push[5] = {0x68};
    uintptr_t to = (uintptr_t)enum_device;
    memcpy(push + 1, &to, 4);
    if (lm_patch(ENUM_PUSH, expect, push, sizeof push)) {
        lm_log("input: DirectInput 8 device types turned into DirectInput 7 ones for the engine");
    } else {
        lm_log("input: the engine's device enumeration is not as expected, left alone");
    }
}
