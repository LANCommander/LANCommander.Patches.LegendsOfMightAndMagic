/* ASI plugin build (lommpatch.asi), for an ASI loader such as Ultimate ASI Loader: the loader LoadLibrary()s every .asi
 * it finds (game folder, scripts\, plugins\) before the program runs its own code, and DllMain applies the same patches
 * as the dinput.dll proxy, with the LANCommander Interposer embedded the same way. The settings come from lommpatch.ini
 * next to the .asi, the log goes to lommpatch.log there.
 *
 * This is also how the patch gets into the dedicated server (LegendServ.exe, which does not load dinput.dll): an ASI
 * loader deployed as version.dll next to it. Without the dinput.dll proxy the DirectInput fix needs the Interposer's
 * own DirectInput hooks (DirectInput.FixLegacyDeviceEnumeration also works when the Interposer is not dinput.dll, for
 * a game that loads dinput.dll after it). */
#include <windows.h>
#include "lommpatch.h"

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
