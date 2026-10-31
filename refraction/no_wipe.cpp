// The G-Man behind glass. A material attribute of Half-Life: Alyx, "no-wipe" (only the G-Man's jacket and briefcase
// carry it), keeps a model out of the scene wipe of the G-Man's scenes: where the rendering pipeline reads a model's
// materials (renderingpipelines.dll, the 6 June 2020 build, fn 0xF97E) it gives that model's scene object a renderable
// flag of its own (bit 48), and the pipeline draws such objects in their own layers, after the others. Their
// see-through parts (the hair, the eyes) then draw after a shader compiler's refracting glass, at their true place
// over the glass's bent copy of the head: a lock of hair beside it. S2FM plays no wipe, so the flag is never given
// here: the jump past it at 0xF9EF (no "no-wipe" seen) is always taken, and the G-Man is drawn as every other
// character is.
//
// The flag is given when a model's scene object is made, so a change is for the models made after it.
// Dev word (s2fm_mods_dev.txt, read at start): nowipefix   the pipeline left as the game has it.
#include <string.h>

#include "../core/core.h"

namespace {

const uint32_t kStamp2020 = 0x5EE41F37;
// test r12b, r12b; je +0x36 (past the flag); mov rax, [r13 + 0x88]; mov rdx, 1 << 48
const uint32_t kNoWipeTest = 0xF9EC;
const uint8_t kNoWipeBytes[] = { 0x45, 0x84, 0xE4, 0x74, 0x36, 0x49, 0x8B, 0x85, 0x88, 0x00, 0x00, 0x00,
                                 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 };
// Where the attribute is read: mov r8d, the hash of "no-wipe".
const uint32_t kNoWipeHash = 0xF8CC;
const uint8_t kNoWipeHashBytes[] = { 0x41, 0xB8, 0xEF, 0x0B, 0x82, 0x9C };

void InstallNoWipeFix() {
    static bool done = false;
    if (done) return;
    HMODULE mod = GetModuleHandleA("renderingpipelines.dll");
    if (!mod) return;
    done = true;
    uint8_t* base = (uint8_t*)mod;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kStamp2020 || memcmp(base + kNoWipeTest, kNoWipeBytes, sizeof(kNoWipeBytes)) ||
        memcmp(base + kNoWipeHash, kNoWipeHashBytes, sizeof(kNoWipeHashBytes))) {
        Log("[NOWIPE] renderingpipelines.dll is not the 6 June 2020 build: \"no-wipe\" models still draw after the glass");
        return;
    }
    uint8_t* site = base + kNoWipeTest + 3;   // the je
    DWORD was = 0;
    if (!VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &was)) {
        Log("[NOWIPE] the pipeline COULD NOT be changed: \"no-wipe\" models still draw after the glass");
        return;
    }
    site[0] = 0xEB;   // jmp: never the flag
    VirtualProtect(site, 1, was, &was);
    FlushInstructionCache(GetCurrentProcess(), site, 1);
    Log("[NOWIPE] \"no-wipe\" materials (the G-Man's) no longer move their models into the wipe's later layers");
}

}  // namespace

bool CoreDevNoWipeFix();   // core/core_dev.cpp

void NoWipeRegister() {
    if (CoreDevNoWipeFix()) {
        Log("[DEV] s2fm_mods_dev.txt says nowipefix: \"no-wipe\" models draw in the wipe's layers as the game has it");
        return;
    }
    CoreOnModuleLoad(InstallNoWipeFix);
}
