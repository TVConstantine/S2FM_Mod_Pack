// Two faults of the asset system's up-to-date check (the 6 May 2022 assetsystem.dll) that make an addon's material of
// a custom shader compile again on every load, about ten times a second, each time reloading what uses it (the
// particles that freeze). The check runs only for the addon's own assets.
//
// 1. The name table wraps. The names of every asset's special dependencies (a material's shader, a compiler's
//    version) live in one symbol table with 16-bit ids, the same table that holds the extra strings of every compiled
//    file. It is saved with the addon's tools_asset_info.bin and holds what the addon's assets need: an addon of a
//    million and more assets fills it past 65535 names, and a rebuilt one fills it the same. A name added after that
//    gets the table's size cut to 16 bits as its id, which is an older, unrelated name's: a material of a shader the
//    table has not seen before is recorded as depending on that name, whose fingerprint never matches. So:
//    - where the compiled file's special dependencies are read in (the REDI block, fn 0x3D2B0), a name whose id comes
//      back as a different name's is given the table's empty id (0xFFFF, which the table reads as "") instead;
//    - where a dependency is checked (0x15E77, the call to the compiler's fingerprint), a dependency with the empty
//      id is taken as up to date: its stored fingerprint is answered. Every other dependency is checked as before.
//    A name kept out of the table is never checked, so a change to it does not recompile its assets by itself; the
//    fingerprint of a shader of ours is its format version, which does not change, so nothing is lost for them.
// 2. A file in core's or hlvr's content that the asset system has no record of (it knows those mods only from the
//    lists shipped with the game) is taken as missing without a look at the disk when its kind is an asset kind (an
//    image is): an addon's material on a texture installed into core content is always out of date. The shortcut
//    (the jump at 0x15A37 after the kind's lookup) is taken out, so such a file is looked for on the disk, in the
//    content folders from the asset's own mod on (fn 0x247D0), as any other unrecorded file is. A file really gone
//    is still missing.
#include <atomic>
#include <string.h>
#include <intrin.h>

#include "../core/core.h"

namespace {

const uint32_t kStamp2022 = 0x6275AD06;
// The table's add (table, id out, name), its read (table, id) and where the REDI reader calls the add for a special
// dependency's compiler and for its name: the returns are the addresses after those two calls.
const uint32_t kAddSymbolFn = 0x170D90, kSymbolNameFn = 0x171180, kRetCompiler = 0x3D81E, kRetName = 0x3D85C;
const uint8_t kAddSymbolStart[12] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x4D, 0x85, 0xC0 };
// The check: the entry is [r14 + rdi] (compiler id +0, name id +2, user data +4, fingerprint +8), the call at 0x15E77
// is call [r10 + 0x80] (this, compiler, name, user data, &valid) and its answer is compared with the fingerprint.
const uint32_t kCheckCall = 0x15E77;
const uint8_t kCheckBytes[] = { 0x41, 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00,   // call qword ptr [r10 + 0x80]
                                0x41, 0x3B, 0x44, 0x3E, 0x08 };             // cmp eax, [r14 + rdi + 8]
const uint16_t kNoName = 0xFFFF;
// The unrecorded file's shortcut, after the call to the kind's lookup: test rax, rax; jne (taken as missing) at 0x15A37.
const uint32_t kUnrecordedTest = 0x15A34;
const uint8_t kUnrecordedBytes[] = { 0x48, 0x85, 0xC0, 0x75, 0x42 };

typedef uint16_t*(__fastcall* AddSymbolFn)(void* table, uint16_t* id, const char* name, void* a4);
typedef const char*(__fastcall* SymbolNameFn)(void* table, const uint16_t* id);
AddSymbolFn g_origAddSymbol = nullptr;
SymbolNameFn g_symbolName = nullptr;
uintptr_t g_retCompiler = 0, g_retName = 0;
std::atomic<long long> g_keptOut{0};

// Four arguments forwarded, whatever the function names (it reads three).
uint16_t* __fastcall AddSymbolHook(void* table, uint16_t* id, const char* name, void* a4) {
    uint16_t* out = g_origAddSymbol(table, id, name, a4);
    const uintptr_t from = (uintptr_t)_ReturnAddress();
    if ((from != g_retCompiler && from != g_retName) || !name || !id || *id == kNoName) return out;
    const char* has = g_symbolName(table, id);
    if (has && _stricmp(has, name) == 0) return out;
    // The id is an older name's: the table is full.
    const long long n = g_keptOut.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || (n & (n - 1)) == 0)
        Log("[ASSETS] the name table is full: special dependency '%s' kept out of it (its id was '%s's), %lld so far",
            name, has ? has : "", n);
    *id = kNoName;
    return out;
}

// The check's stand in, in machine code, called where the engine calls the fingerprint. An entry with the empty id for
// its name or its compiler: valid, and the stored fingerprint. Any other: on to the engine's own call, which returns
// to the check as it always did.
const uint8_t kThunk[] = {
    0x41, 0x0F, 0xB7, 0x44, 0x3E, 0x02,         // movzx eax, word ptr [r14 + rdi + 2]   (name)
    0x66, 0x83, 0xF8, 0xFF,                     // cmp ax, 0xFFFF
    0x74, 0x13,                                 // je kept
    0x41, 0x0F, 0xB7, 0x44, 0x3E, 0x00,         // movzx eax, word ptr [r14 + rdi + 0]   (compiler)
    0x66, 0x83, 0xF8, 0xFF,                     // cmp ax, 0xFFFF
    0x74, 0x07,                                 // je kept
    0x41, 0xFF, 0xA2, 0x80, 0x00, 0x00, 0x00,   // jmp qword ptr [r10 + 0x80]
    // kept:
    0x48, 0x8B, 0x44, 0x24, 0x28,               // mov rax, [rsp + 0x28]                 (&valid, past our return)
    0x48, 0x85, 0xC0,                           // test rax, rax
    0x74, 0x03,                                 // je +3
    0xC6, 0x00, 0x01,                           // mov byte ptr [rax], 1
    0x41, 0x8B, 0x44, 0x3E, 0x08,               // mov eax, [r14 + rdi + 8]              (the stored fingerprint)
    0xC3,                                       // ret
};

// A page within a rel32 call of the module, for the stand in.
uint8_t* PageNear(uint8_t* base) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity, center = (uintptr_t)base;
    for (uintptr_t d = step; d < 0x70000000; d += step) {
        for (int sign = 0; sign < 2; ++sign) {
            const uintptr_t at = sign ? center + d : center - d;
            if (!sign && d > center) continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)(at & ~(step - 1)), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) continue;
            if (void* p = VirtualAlloc((void*)(at & ~(step - 1)), si.dwPageSize, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE))
                return (uint8_t*)p;
        }
    }
    return nullptr;
}

bool PatchCheck(uint8_t* base) {
    uint8_t* page = PageNear(base);
    if (!page) return false;
    memcpy(page, kThunk, sizeof(kThunk));
    DWORD was = 0;
    VirtualProtect(page, sizeof(kThunk), PAGE_EXECUTE_READ, &was);
    FlushInstructionCache(GetCurrentProcess(), page, sizeof(kThunk));
    uint8_t* site = base + kCheckCall;
    const intptr_t rel = (intptr_t)page - (intptr_t)(site + 5);
    if (rel != (int32_t)rel) return false;
    uint8_t call[7] = { 0xE8, 0, 0, 0, 0, 0x66, 0x90 };   // call stand in; xchg ax, ax
    const int32_t rel32 = (int32_t)rel;
    memcpy(call + 1, &rel32, 4);
    if (!VirtualProtect(site, sizeof(call), PAGE_EXECUTE_READWRITE, &was)) return false;
    memcpy(site, call, sizeof(call));
    VirtualProtect(site, sizeof(call), was, &was);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(call));
    return true;
}

bool PatchUnrecorded(uint8_t* base) {
    uint8_t* site = base + kUnrecordedTest + 3;   // the jne
    DWORD was = 0;
    if (!VirtualProtect(site, 2, PAGE_EXECUTE_READWRITE, &was)) return false;
    site[0] = 0x66;   // xchg ax, ax: on to the look on the disk
    site[1] = 0x90;
    VirtualProtect(site, 2, was, &was);
    FlushInstructionCache(GetCurrentProcess(), site, 2);
    return true;
}

void InstallDependencyFixes() {
    static bool done = false;
    if (done) return;
    HMODULE mod = GetModuleHandleA("assetsystem.dll");
    if (!mod) return;
    done = true;
    uint8_t* base = (uint8_t*)mod;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kStamp2022 || memcmp(base + kAddSymbolFn, kAddSymbolStart, sizeof(kAddSymbolStart)) ||
        memcmp(base + kCheckCall, kCheckBytes, sizeof(kCheckBytes)) ||
        memcmp(base + kUnrecordedTest, kUnrecordedBytes, sizeof(kUnrecordedBytes))) {
        Log("[ASSETS] assetsystem.dll is not the 6 May 2022 build: its up-to-date check is left as it is");
        return;
    }
    Log("[ASSETS] unrecorded files of core and hlvr %s", PatchUnrecorded(base) ? "are looked for on the disk"
                                                                                : "COULD NOT be changed: still taken as missing");
    g_symbolName = (SymbolNameFn)(base + kSymbolNameFn);
    g_retCompiler = (uintptr_t)base + kRetCompiler;
    g_retName = (uintptr_t)base + kRetName;
    // The check first: a name kept out before the check knows it would be checked against "".
    if (!PatchCheck(base)) {
        Log("[ASSETS] the dependency check COULD NOT be changed: the name table is left as it is");
        return;
    }
    const bool ok = CoreHook(base + kAddSymbolFn, (void*)&AddSymbolHook, (void**)&g_origAddSymbol);
    Log("[ASSETS] %s", ok ? "names past the full name table are kept out of it, and their dependencies taken as up to date"
                          : "the name table's add COULD NOT be hooked: a full table still mixes names up");
}

}  // namespace

bool CoreDevNoAssetFix();   // core/core_dev.cpp

void AssetDepsRegister() {
    if (CoreDevNoAssetFix()) {
        Log("[DEV] s2fm_mods_dev.txt says noassetfix: the asset system's up-to-date check is left as it is");
        return;
    }
    CoreOnModuleLoad(InstallDependencyFixes);
}
