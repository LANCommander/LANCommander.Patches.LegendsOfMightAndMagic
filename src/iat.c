/* Import address table hooks (the exe, or one of the game's DLLs). */
#include <string.h>
#include "lommpatch.h"

void *lm_hook_import(const char *dll, const char *name, void *replacement)
{
    return lm_hook_import_in(GetModuleHandleA(NULL), dll, name, replacement);
}

static IMAGE_IMPORT_DESCRIPTOR *imports(HMODULE module)
{
    char *base = (char *)module;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    return dir->VirtualAddress ? (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress) : NULL;
}

void *lm_hook_import_in(HMODULE module, const char *dll, const char *name, void *replacement)
{
    char *base = (char *)module;
    IMAGE_IMPORT_DESCRIPTOR *imp = module ? imports(module) : NULL;
    for (; imp && imp->Name; imp++) {
        IMAGE_THUNK_DATA32 *names, *slots;
        if (lstrcmpiA(base + imp->Name, dll) != 0) {
            continue;
        }
        slots = (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
        if (!imp->OriginalFirstThunk) {
            /* no name table (the bound IAT is all that is left): match the resolved address */
            HMODULE mod = GetModuleHandleA(dll);
            uintptr_t target = mod ? (uintptr_t)GetProcAddress(mod, name) : 0;
            for (; target && slots->u1.Function; slots++) {
                if (slots->u1.Function == target) {
                    lm_write((uintptr_t)&slots->u1.Function, &replacement, 4);
                    return (void *)target;
                }
            }
            continue;
        }
        names = (IMAGE_THUNK_DATA32 *)(base + imp->OriginalFirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            void *old;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                /* by ordinal: match the address the name resolves to */
                HMODULE mod = GetModuleHandleA(dll);
                if (!mod || slots->u1.Function != (uintptr_t)GetProcAddress(mod, name)) {
                    continue;
                }
            } else if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name) != 0) {
                continue;
            }
            old = (void *)(uintptr_t)slots->u1.Function;
            lm_write((uintptr_t)&slots->u1.Function, &replacement, 4);
            return old;
        }
    }
    lm_log("iat: %s!%s not imported", dll, name);
    return NULL;
}

/* WSOCK32 is imported by ordinal (sendto = 20) by the game and its DLLs */
void *lm_hook_import_ordinal(HMODULE module, const char *dll, WORD ordinal, void *replacement)
{
    char *base = (char *)module;
    IMAGE_IMPORT_DESCRIPTOR *imp = module ? imports(module) : NULL;
    for (; imp && imp->Name; imp++) {
        IMAGE_THUNK_DATA32 *names, *slots;
        if (lstrcmpiA(base + imp->Name, dll) != 0 || !imp->OriginalFirstThunk) {
            continue;
        }
        slots = (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
        names = (IMAGE_THUNK_DATA32 *)(base + imp->OriginalFirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            void *old;
            if (!IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal) || IMAGE_ORDINAL32(names->u1.Ordinal) != ordinal) {
                continue;
            }
            old = (void *)(uintptr_t)slots->u1.Function;
            lm_write((uintptr_t)&slots->u1.Function, &replacement, 4);
            return old;
        }
    }
    lm_log("iat: %s!#%u not imported", dll, ordinal);
    return NULL;
}
