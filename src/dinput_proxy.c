/* dinput.dll proxy: lithtech.exe imports DINPUT.dll!DirectInputCreateA, so Windows loads this file from the game
 * folder first. DllMain attaches the embedded LANCommander Interposer and applies the patches before lithtech.exe's
 * entry point runs. DirectInputCreateA / Ex go through the Interposer's InterposerDirectInputCreateA / Ex, which keep
 * DirectInput.FixLegacyDeviceEnumeration in effect: modern Windows' dinput.dll overruns its device array while it
 * enumerates HID devices (with enough of them plugged in the game hangs or crashes at startup), and the Interposer
 * serves the game a DirectInput 8 object instead (the Config.yml the patch writes turns it on, interposer_config.c).
 * With it off they call the real dinput.dll in the system folder. */
#include <windows.h>
#include "lommpatch.h"
#include "interposer/LANCommander.Interposer.h"

static FARPROC real_proc(const char *name)
{
    static HMODULE real;
    if (!real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   /* SysWOW64 for a 32-bit process */
        lstrcpynA(path + n, "\\dinput.dll", MAX_PATH - n);
        real = LoadLibraryA(path);
        if (!real) {
            lm_log("cannot load %s (%lu)", path, GetLastError());
            return NULL;
        }
    }
    return GetProcAddress(real, name);
}

#define REAL(name, type) static type real; if (!real) { real = (type)(void (*)(void))real_proc(#name); if (!real) return E_FAIL; }

typedef HRESULT (WINAPI *DirectInputCreate_fn)(HINSTANCE, DWORD, void **, void *);
typedef HRESULT (WINAPI *DllGetClassObject_fn)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *Void_fn)(void);

HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    HRESULT hr = InterposerDirectInputCreateA(inst, ver, out, outer);
    lm_log("dinput: DirectInputCreateA(version %lx) = %08lx", ver, (unsigned long)hr);
    return hr;
}

HRESULT WINAPI DirectInputCreateW(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    REAL(DirectInputCreateW, DirectInputCreate_fn);
    return real(inst, ver, out, outer);
}

HRESULT WINAPI DirectInputCreateEx(HINSTANCE inst, DWORD ver, REFIID iid, void **out, void *outer)
{
    return InterposerDirectInputCreateEx(inst, ver, iid, out, outer);
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    REAL(DllGetClassObject, DllGetClassObject_fn);
    return real(clsid, iid, out);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;   /* the patch stays loaded for the life of the process */
}

HRESULT WINAPI DllRegisterServer(void)
{
    REAL(DllRegisterServer, Void_fn);
    return real();
}

HRESULT WINAPI DllUnregisterServer(void)
{
    REAL(DllUnregisterServer, Void_fn);
    return real();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        lm_standalone_init(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        lm_standalone_detach(reserved != NULL);
    }
    return TRUE;
}
