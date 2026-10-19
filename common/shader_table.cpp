// The shader table's reader. See shader_table.h, and shader_table_build.cpp for the file.
//
// The file is wherever ShaderDbSetPath says (beside the DLL), and
// is looked for again, once a second at most, until it is there: on a first start it is still being made while the
// game loads its shaders (shader objects made before it is ready are named when they are first bound).
//
// Read once into memory and left there: about 39 MB, mostly the 335 thousand entries, which are sorted by checksum
// so a lookup is a binary search.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "shader_table.h"

void Log(const char* fmt, ...);

namespace {

std::wstring g_Path;
const uint32_t kVersion = 3;

struct Entry {
    uint8_t checksum[16];
    uint32_t layout;
    uint32_t combo;
    uint16_t flags;
    uint16_t pad;
};
static_assert(sizeof(Entry) == 28, "entry layout");

// A block of records and the offset of each into it.
struct Block {
    std::vector<uint32_t> offsets;
    const uint8_t* data = nullptr;
    size_t size = 0;

    // The start of a record, with at least `need` bytes of it inside the block, or null.
    const uint8_t* At(int i, size_t need) const {
        if (i < 0 || (size_t)i >= offsets.size()) return nullptr;
        const size_t off = offsets[i];
        if (off + need > size) return nullptr;
        return data + off;
    }
    bool Has(const uint8_t* p, size_t need) const { return p && (size_t)(p - data) + need <= size; }
};

std::mutex g_LoadMutex;
std::atomic<bool> g_Ready{false};
ULONGLONG g_LastTry = 0;
std::vector<uint8_t> g_File;
std::vector<std::string> g_Names;
// Each name's hash, see ShaderDbHash, worked out once at load.
std::vector<uint32_t> g_Hashes;
Block g_Layouts, g_AxisSets, g_Combos;
const Entry* g_Entries = nullptr;
uint32_t g_EntryCount = 0;

uint16_t Rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t Rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

const char* Str(uint16_t i) { return i < g_Names.size() ? g_Names[i].c_str() : ""; }
uint32_t Hash(uint16_t i) { return i < g_Hashes.size() ? g_Hashes[i] : 0u; }

bool ReadBlock(const uint8_t*& p, const uint8_t* end, uint32_t count, Block& b) {
    if ((size_t)(end - p) < (size_t)count * 4 + 4) return false;
    b.offsets.resize(count);
    for (uint32_t i = 0; i < count; ++i) b.offsets[i] = Rd32(p + 4 * i);
    p += (size_t)count * 4;
    b.size = Rd32(p);
    p += 4;
    if ((size_t)(end - p) < b.size) return false;
    b.data = p;
    p += b.size;
    return true;
}

// Under g_LoadMutex. False leaves nothing loaded, for the next try.
bool Load() {
    FILE* f = nullptr;
    if (g_Path.empty() || _wfopen_s(&f, g_Path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_File.resize(size > 0 ? (size_t)size : 0);
    const size_t got = g_File.empty() ? 0 : fread(g_File.data(), 1, g_File.size(), f);
    fclose(f);
    g_Names.clear();
    if (got != g_File.size() || g_File.size() < 28 || memcmp(g_File.data(), "DXRS", 4) != 0
        || Rd32(g_File.data() + 4) != kVersion) {
        Log("[TABLE] the shader table is not a version %u table, shaders are not named\n", kVersion);
        g_File.clear();
        return false;
    }
    const uint8_t* p = g_File.data();
    const uint8_t* end = p + g_File.size();
    const uint32_t nStrings = Rd32(p + 8), nLayouts = Rd32(p + 12), nAxisSets = Rd32(p + 16);
    const uint32_t nCombos = Rd32(p + 20), nEntries = Rd32(p + 24);
    p += 28;
    g_Names.reserve(nStrings);
    for (uint32_t i = 0; i < nStrings; ++i) {
        if (p + 2 > end) return false;
        const uint16_t len = Rd16(p);
        p += 2;
        if (p + len > end) return false;
        g_Names.emplace_back((const char*)p, len);
        p += len;
    }
    if (!ReadBlock(p, end, nLayouts, g_Layouts) || !ReadBlock(p, end, nAxisSets, g_AxisSets)
        || !ReadBlock(p, end, nCombos, g_Combos)) {
        Log("[TABLE] the shader table is cut short, shaders are not named\n");
        return false;
    }
    if ((size_t)(end - p) != (size_t)nEntries * sizeof(Entry)) {
        Log("[TABLE] the shader table has %zu bytes where its %u entries should be, shaders are not named\n",
            (size_t)(end - p), nEntries);
        return false;
    }
    g_Entries = (const Entry*)p;
    g_EntryCount = nEntries;
    g_Hashes.resize(g_Names.size());
    for (size_t i = 0; i < g_Names.size(); ++i) g_Hashes[i] = ShaderDbHash(g_Names[i].c_str());
    Log("[TABLE] loaded: %u compiled shaders, %u parameter layouts, %u checkbox sets, %u names, %.1f MB\n", nEntries,
        nLayouts, nCombos, nStrings, (double)g_File.size() / (1024.0 * 1024.0));
    return true;
}

bool Ready() {
    if (g_Ready.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lk(g_LoadMutex);
    if (g_Ready.load(std::memory_order_relaxed)) return true;
    const ULONGLONG now = GetTickCount64();
    if (g_LastTry && now - g_LastTry < 1000) return false;
    g_LastTry = now;
    if (Load()) g_Ready.store(true, std::memory_order_release);
    return g_Ready.load(std::memory_order_relaxed);
}

}  // namespace

void ShaderDbSetPath(const wchar_t* path) {
    std::lock_guard<std::mutex> lk(g_LoadMutex);
    g_Path = path ? path : L"";
    g_LastTry = 0;
}

bool ShaderDbReady() { return Ready(); }

namespace {

// A layout's record: u16 shader, u8 stage, u16 fields, then 6 bytes a field.
const uint8_t* LayoutAt(int layout) {
    return Ready() ? g_Layouts.At(layout, 5) : nullptr;
}

// A combo's record: u16 axis set, u16 axes, a byte a value.
const uint8_t* ComboAt(int combo) {
    return Ready() ? g_Combos.At(combo, 4) : nullptr;
}

}  // namespace

int ShaderDbVarDwords(uint8_t t) {
    if (t >= SDB_VAR_FLOAT && t <= SDB_VAR_FLOAT4) return t - SDB_VAR_FLOAT + 1;
    if (t >= SDB_VAR_INT && t <= SDB_VAR_INT4) return t - SDB_VAR_INT + 1;
    if (t >= SDB_VAR_BOOL && t <= SDB_VAR_BOOL4) return t - SDB_VAR_BOOL + 1;
    return 0;
}

const char* ShaderDbVarTypeName(uint8_t t) {
    static const char* const names[] = { "void", "float", "float2", "float3", "float4", "int",
        "int2", "int3", "int4", "bool", "bool2", "bool3", "bool4", "sampler1D", "sampler2D",
        "sampler3D", "samplerCube" };
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "other";
}

int ShaderDbLookup(const void* bytecode, size_t size, uint16_t* psFlags, int* combo) {
    if (combo) *combo = -1;
    if (!bytecode || size < 20 || memcmp(bytecode, "DXBC", 4) != 0 || !Ready()) return -1;
    const uint8_t* key = (const uint8_t*)bytecode + 4;
    uint32_t lo = 0, hi = g_EntryCount;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const Entry& e = g_Entries[mid];
        const int c = memcmp(e.checksum, key, 16);
        if (c == 0) {
            if (psFlags) *psFlags = e.flags;
            if (combo) *combo = e.combo == 0xFFFFFFFFu ? -1 : (int)e.combo;
            return (int)e.layout;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

uint32_t ShaderDbHash(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

uint32_t ShaderDbNameHash(int layout) {
    const uint8_t* r = LayoutAt(layout);
    return r ? Hash(Rd16(r)) : 0u;
}

bool ShaderDbFieldHashed(int layout, int index, uint32_t* nameHash, uint8_t* regType,
                         uint16_t* offset, uint8_t* varType) {
    const uint8_t* r = LayoutAt(layout);
    if (!r || index < 0 || index >= (int)Rd16(r + 3)) return false;
    const uint8_t* f = r + 5 + (size_t)index * 6;
    if (!g_Layouts.Has(f, 6)) return false;
    if (nameHash) *nameHash = Hash(Rd16(f));
    if (regType) *regType = f[2];
    if (offset) *offset = Rd16(f + 3);
    if (varType) *varType = f[5];
    return true;
}

const char* ShaderDbName(int layout) {
    const uint8_t* r = LayoutAt(layout);
    return r ? Str(Rd16(r)) : "";
}

int ShaderDbFieldCount(int layout) {
    const uint8_t* r = LayoutAt(layout);
    return r ? (int)Rd16(r + 3) : 0;
}

bool ShaderDbField(int layout, int index, const char** name, uint8_t* regType, uint16_t* offset,
                   uint8_t* varType) {
    const uint8_t* r = LayoutAt(layout);
    if (!r || index < 0 || index >= (int)Rd16(r + 3)) return false;
    const uint8_t* f = r + 5 + (size_t)index * 6;
    if (!g_Layouts.Has(f, 6)) return false;
    if (name) *name = Str(Rd16(f));
    if (regType) *regType = f[2];
    if (offset) *offset = Rd16(f + 3);
    if (varType) *varType = f[5];
    return true;
}

bool ShaderDbFind(int layout, const char* name, uint8_t* regType, uint16_t* offset,
                  uint8_t* varType) {
    const int n = ShaderDbFieldCount(layout);
    for (int i = 0; i < n; ++i) {
        const char* fn = nullptr;
        uint8_t rt = 0, vt = 0;
        uint16_t off = 0;
        if (ShaderDbField(layout, i, &fn, &rt, &off, &vt) && fn && strcmp(fn, name) == 0) {
            if (regType) *regType = rt;
            if (offset) *offset = off;
            if (varType) *varType = vt;
            return true;
        }
    }
    return false;
}

int ShaderDbComboAxisCount(int combo) {
    const uint8_t* c = ComboAt(combo);
    return c ? (int)Rd16(c + 2) : 0;
}

bool ShaderDbComboAxis(int combo, int index, const char** axis, const char** feature,
                       const char** stateNames, int* value) {
    const uint8_t* c = ComboAt(combo);
    if (!c || index < 0 || index >= (int)Rd16(c + 2) || !g_Combos.Has(c + 4 + index, 1)) return false;
    const uint8_t* s = g_AxisSets.At(Rd16(c), 5);
    if (!s || index >= (int)Rd16(s + 3)) return false;
    const uint8_t* a = s + 5 + (size_t)index * 6;
    if (!g_AxisSets.Has(a, 6)) return false;
    const uint16_t fs = Rd16(a + 2), ss = Rd16(a + 4);
    if (axis) *axis = Str(Rd16(a));
    if (feature) *feature = fs == 0xFFFF ? "" : Str(fs);
    if (stateNames) *stateNames = ss == 0xFFFF ? "" : Str(ss);
    if (value) *value = c[4 + index] == 0xFF ? -2 : (int)c[4 + index];
    return true;
}

bool ShaderDbComboAxisHashed(int combo, int index, uint32_t* axisHash, uint32_t* featureHash,
                             int* value) {
    const uint8_t* c = ComboAt(combo);
    if (!c || index < 0 || index >= (int)Rd16(c + 2) || !g_Combos.Has(c + 4 + index, 1)) return false;
    const uint8_t* s = g_AxisSets.At(Rd16(c), 5);
    if (!s || index >= (int)Rd16(s + 3)) return false;
    const uint8_t* a = s + 5 + (size_t)index * 6;
    if (!g_AxisSets.Has(a, 6)) return false;
    const uint16_t fs = Rd16(a + 2);
    if (axisHash) *axisHash = Hash(Rd16(a));
    if (featureHash) *featureHash = fs == 0xFFFF ? 0u : Hash(fs);
    if (value) *value = c[4 + index] == 0xFF ? -2 : (int)c[4 + index];
    return true;
}

int ShaderDbComboValue(int combo, const char* name) {
    const int n = ShaderDbComboAxisCount(combo);
    for (int i = 0; i < n; ++i) {
        const char* axis = nullptr;
        const char* feature = nullptr;
        int v = -1;
        if (ShaderDbComboAxis(combo, i, &axis, &feature, nullptr, &v)
            && (strcmp(axis, name) == 0 || strcmp(feature, name) == 0)) {
            return v;
        }
    }
    return -1;
}
