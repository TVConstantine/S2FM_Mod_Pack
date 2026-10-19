// The shader table, from the game's own .vcs files. See shader_table_build.h.
//
// The format is the one ValveResourceFormat reads (CompiledShader/, VCS version 64
// for Half-Life: Alyx); each static combo is LZMA compressed.
#include "shader_table_build.h"
#include "lzma_decode.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace {

struct Bad {};   // a file that does not read as it should; its program is left out, as the Python leaves it out

struct Reader {
    const uint8_t* b;
    size_t n, p;
    Reader(const uint8_t* data, size_t size, size_t pos = 0) : b(data), n(size), p(pos) {}
    void Need(size_t k) const { if (p + k > n || p + k < p) throw Bad(); }
    int32_t I32() { Need(4); int32_t v; memcpy(&v, b + p, 4); p += 4; return v; }
    uint32_t U32() { Need(4); uint32_t v; memcpy(&v, b + p, 4); p += 4; return v; }
    int16_t I16() { Need(2); int16_t v; memcpy(&v, b + p, 2); p += 2; return v; }
    uint16_t U16() { Need(2); uint16_t v; memcpy(&v, b + p, 2); p += 2; return v; }
    int64_t I64() { Need(8); int64_t v; memcpy(&v, b + p, 8); p += 8; return v; }
    float F32() { Need(4); float v; memcpy(&v, b + p, 4); p += 4; return v; }
    uint8_t U8() { Need(1); return b[p++]; }
    void Skip(int64_t k) { if (k < 0) throw Bad(); Need((size_t)k); p += (size_t)k; }
    std::string Cstr() {
        const void* z = p < n ? memchr(b + p, 0, n - p) : nullptr;
        if (!z) throw Bad();
        const size_t e = (const uint8_t*)z - b;
        std::string s((const char*)b + p, e - p);
        p = e + 1;
        return s;
    }
    std::string Fixed(size_t width) {
        std::string s = Cstr();
        if (s.size() + 1 > width) throw Bad();   // "string longer than its field"
        Skip((int64_t)(width - s.size() - 1));
        return s;
    }
};

// --- The archives.
struct Entry {
    std::wstring dirPath;
    uint16_t arch = 0;
    uint32_t off = 0, length = 0;
    std::vector<uint8_t> preload;
    uint32_t dirEnd = 0;
};

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    _fseeki64(f, 0, SEEK_END);
    const int64_t size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize((size_t)(size > 0 ? size : 0));
    const size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

// The entries of a directory archive, in the order its tree lists them, keyed by name and extension (the folder
// is not part of the key, as in the Python).
bool VpkEntries(const std::wstring& path, std::vector<std::string>& order, std::unordered_map<std::string, Entry>& out) {
    std::vector<uint8_t> b;
    if (!ReadFileAll(path, b) || b.size() < 12) return false;
    try {
        Reader r(b.data(), b.size());
        const uint32_t sig = r.U32(), ver = r.U32(), tree = r.U32();
        if (sig != 0x55AA1234) return false;
        const uint32_t hdr = ver == 1 ? 12 : 28;
        r.p = hdr;
        for (;;) {
            const std::string ext = r.Cstr();
            if (ext.empty()) break;
            for (;;) {
                const std::string dir = r.Cstr();
                if (dir.empty()) break;
                for (;;) {
                    const std::string name = r.Cstr();
                    if (name.empty()) break;
                    r.U32();   // crc
                    Entry e;
                    const uint16_t preload = r.U16();
                    e.arch = r.U16();
                    e.off = r.U32();
                    e.length = r.U32();
                    r.U16();   // terminator
                    r.Need(preload);
                    e.preload.assign(b.data() + r.p, b.data() + r.p + preload);
                    r.p += preload;
                    e.dirPath = path;
                    e.dirEnd = hdr + tree;
                    const std::string full = name + "." + ext;
                    if (!out.count(full)) order.push_back(full);
                    out[full] = std::move(e);
                }
            }
        }
    } catch (const Bad&) {
        return false;
    }
    return true;
}

bool VpkRead(const Entry& e, std::vector<uint8_t>& out) {
    std::wstring path = e.dirPath;
    uint64_t at = e.off;
    if (e.arch == 0x7FFF) {
        at += e.dirEnd;
    } else {
        const size_t k = path.rfind(L"_dir.vpk");
        if (k == std::wstring::npos) return false;
        wchar_t num[16];
        swprintf_s(num, L"_%03u.vpk", (unsigned)e.arch);
        path = path.substr(0, k) + num;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    out = e.preload;
    out.resize(e.preload.size() + e.length);
    _fseeki64(f, (int64_t)at, SEEK_SET);
    const size_t got = e.length ? fread(out.data() + e.preload.size(), 1, e.length, f) : 0;
    fclose(f);
    out.resize(e.preload.size() + got);   // a short read is what the Python's f.read gives too
    return true;
}

// --- A program's blocks, as much of them as the table needs.
struct Combo { std::string name; int32_t min = 0, max = 0, feature = -1; std::vector<std::string> states; };
struct Variable { std::string name; int32_t reg = 0, type = 0; };
struct Field { uint16_t var = 0, raw = 0; };
struct IndexArray { std::vector<Field> fields; };
struct State {
    int64_t dyn = 0;
    int32_t file = -1;
    bool hasBlend = false, hasDepth = false;
    uint8_t a2c = 0, enable0 = 0, src0 = 0, dst0 = 0, depthTest = 0, depthWrite = 0;
};
struct StaticCombo {
    IndexArray all;
    std::vector<IndexArray> dynamic;
    std::vector<std::string> checksums;   // per file: the DXBC's bytes 4 to 20, "" when it has fewer than 20
    std::vector<State> states;
};
struct Program {
    int32_t version = 0;
    std::string program;
    std::vector<Combo> statics;
    std::vector<Variable> variables;
    std::vector<std::pair<int64_t, StaticCombo>> combos;
};

Combo ReadCombo(Reader& r) {
    Combo c;
    c.name = r.Fixed(64);
    r.Fixed(64);   // alias
    r.I32();       // type
    c.min = r.I32();
    c.max = r.I32();
    const int32_t source = r.I32();
    c.feature = r.I32();
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n; ++i) c.states.push_back(r.Cstr());
    if (source == 10 || source == 11) r.I32();
    return c;
}

void ReadRule(Reader& r) {
    r.I32();
    r.I32();
    r.Skip(16);
    r.Skip(16 * 4 * 3);
    r.Fixed(256);
}

Variable ReadVariable(Reader& r, int32_t version) {
    Variable v;
    v.name = r.Fixed(64);
    r.Fixed(64);   // group
    r.I32();       // ui
    r.F32();       // step
    r.Fixed(64);   // source string
    const int32_t source = r.I32();
    if (source == 6 || source == 7) r.Skip(r.I32());
    int32_t sourceIndex = r.I32();
    if (sourceIndex == 0x534D4253) {
        r.Skip(r.I32());
        sourceIndex = r.I32();
    }
    v.type = r.I32();
    v.reg = r.I32();
    if (version >= 64) r.I32();
    r.I32();   // elements
    r.I32();   // bits
    r.Fixed(64);
    r.Skip(4 * 4 * 6);   // int and float defaults, minimums, maximums
    r.I32();   // format
    r.I32();   // channels
    r.Skip(4 * 4);
    r.I32();   // colour space
    r.I32();   // precision
    r.Fixed(32);
    r.Fixed(32);
    if (version >= 65) { r.U8(); r.U8(); r.I32(); }
    if (version >= 69) r.U8();
    return v;
}

void ReadChannelProcessor(Reader& r) {
    r.U32();
    r.Skip(16);
    r.I32();
    r.Fixed(256);
}

void ReadCbuffer(Reader& r) {
    r.Fixed(64);
    r.I32();
    r.I32();
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n; ++i) {
        r.Fixed(64);
        r.Skip(16);
    }
    r.U32();
}

void ReadInputSignature(Reader& r) {
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n; ++i) { r.Cstr(); r.Cstr(); r.Cstr(); r.I32(); }
}

IndexArray ReadIndexArray(Reader& r, bool withOffsets) {
    IndexArray a;
    const int32_t count = r.I32();
    r.I32();
    r.I32();
    for (int32_t i = 0; i < count; ++i) {
        Field f;
        const uint16_t packed = (uint16_t)r.I16();
        const uint16_t off = withOffsets ? (uint16_t)r.I16() : 0;
        f.var = packed & 0xFFF;
        f.raw = off;
        a.fields.push_back(f);
    }
    return a;
}

void ReadAttribute(Reader& r) {
    r.Cstr();
    r.U32();
    const uint8_t type = r.U8();
    const int16_t binding = r.I16();
    if (binding != -1) return;
    const int32_t n = r.I32();
    if (n > 0) { r.Skip(n); return; }
    if (type == 1) r.F32();
    else if (type == 5) r.I32();
    else if (type == 9) r.U8();
    else if (type == 41) r.Cstr();
    else if (type >= 2 && type <= 4) r.Skip(4 * type);
    else throw Bad();
}

std::vector<uint8_t> DecompressCombo(const std::vector<uint8_t>& data, int32_t offset, int32_t version,
                                     const std::string& program) {
    Reader r(data.data(), data.size(), (size_t)(uint32_t)offset);
    const int32_t first = r.I32();
    if (version < 64 && program == "features") {
        r.Need((size_t)(uint32_t)first);
        return std::vector<uint8_t>(data.begin() + r.p, data.begin() + r.p + (uint32_t)first);
    }
    int32_t size = r.I32();
    if (size == 0x414D5A4C) {   // "LZMA"
        size = r.I32();
        const int32_t csize = r.I32();
        uint8_t props[5];
        r.Need(5);
        memcpy(props, data.data() + r.p, 5);
        r.p += 5;
        if (size < 0 || csize < 0) throw Bad();
        r.Need((size_t)csize);
        std::vector<uint8_t> out((size_t)size);
        if (!LzmaDecode(props, data.data() + r.p, (size_t)csize, out.data(), out.size())) throw Bad();
        return out;
    }
    if (first == -1) {
        r.p += 4;
        if (size < 0) throw Bad();
        r.Need((size_t)size);
        return std::vector<uint8_t>(data.begin() + r.p, data.begin() + r.p + size);
    }
    throw Bad();
}

StaticCombo ReadStaticCombo(const std::vector<uint8_t>& blob, const std::string& program, int32_t version) {
    Reader r(blob.data(), blob.size());
    StaticCombo sc;
    const bool isFeatures = program == "features";
    const bool isPs = program == "ps" || program == "psrs";
    sc.all = ReadIndexArray(r, !isFeatures);
    for (int16_t i = 0, n = r.I16(); i < n; ++i) ReadAttribute(r);
    if (isFeatures || program == "vs") {
        const int16_t n = r.I16();
        for (int16_t i = 0; i < n; ++i) r.I16();
        if (isFeatures) return sc;
    }
    for (uint16_t i = 0, n = r.U16(); i < n; ++i) sc.dynamic.push_back(ReadIndexArray(r, true));
    for (uint16_t i = 0, n = r.U16(); i < n; ++i) { r.U8(); r.U8(); }
    r.I32();   // cb size
    r.U8();    // static cb
    if (version >= 66) r.U8();
    const int32_t nfiles = r.I32();
    if (version >= 60) r.U8();
    for (int32_t i = 0; i < nfiles; ++i) {
        const int32_t size = r.I32();
        std::string chk;
        if (size > 0) {
            r.Need((size_t)size);
            if (size >= 20) chk.assign((const char*)blob.data() + r.p + 4, 16);
            r.p += size;
        }
        r.Skip(16);   // md5
        sc.checksums.push_back(chk);
    }
    for (int32_t i = 0, n = r.I32(); i < n; ++i) {
        State st;
        st.dyn = r.I64();
        st.file = r.I32();
        r.I32();   // file offset
        if (isPs) {
            const bool hasRs = !r.U8(), hasDs = !r.U8(), hasBs = !r.U8();
            if (hasRs) r.Skip(4 + 4 + 4 + 4);
            if (hasDs) {
                st.hasDepth = true;
                st.depthTest = r.U8();
                st.depthWrite = r.U8();
                r.U8();
                r.Skip(2);
                r.U8();
                r.Skip(2 + 8 + 4);
            }
            if (hasBs) {
                st.hasBlend = true;
                st.a2c = r.U8();
                r.U8();
                r.Skip(1);
                uint8_t enable[8], src[8], dst[8];
                for (uint8_t& x : enable) x = r.U8();
                for (uint8_t& x : src) x = r.U8();
                for (uint8_t& x : dst) x = r.U8();
                r.Skip(8);       // op
                r.Skip(8 * 4);
                r.Skip(8);       // dual source
                st.enable0 = enable[0];
                st.src0 = src[0];
                st.dst0 = dst[0];
            }
        } else if (program == "hs") {
            r.U8();
        }
        sc.states.push_back(st);
    }
    if (r.p != blob.size()) throw Bad();   // "static combo not read to its end"
    return sc;
}

bool ReadProgram(const std::vector<uint8_t>& data, const std::string& fname, Program& p) {
    try {
        Reader r(data.data(), data.size());
        if (r.U32() != 0x32736376) return false;   // "vcs2"
        p.version = r.I32();
        {
            const size_t us = fname.rfind('_');
            std::string tail = fname.substr(us + 1);
            p.program = tail.substr(0, tail.find('.'));
        }
        int count = 7;
        if (p.version >= 68) count -= 2;
        if (p.version < 63) count -= 1;
        if (p.version >= 64) {
            const uint32_t flags = r.U32();
            if (flags & 4) count += 3;
            else if (flags & 2) count += 2;
            else if (flags & 1) count += 1;
        }
        if (p.program == "features") {
            r.I32();
            r.I32();
            r.Cstr();
            r.I32();
            r.Skip(4 * count);
            for (int32_t i = 0, n = r.I32(); i < n; ++i) {
                r.Fixed(64);
                r.Fixed(64);
                for (int32_t j = 0, m = r.I32(); j < m; ++j) { r.Fixed(64); r.I32(); }
            }
            r.Skip(16 * count);
        } else {
            r.Skip(16);
        }
        r.Skip(16);
        r.I32();
        for (int32_t i = 0, n = r.I32(); i < n; ++i) p.statics.push_back(ReadCombo(r));
        for (int32_t i = 0, n = r.I32(); i < n; ++i) ReadRule(r);
        for (int32_t i = 0, n = r.I32(); i < n; ++i) ReadCombo(r);
        for (int32_t i = 0, n = r.I32(); i < n; ++i) ReadRule(r);
        for (int32_t i = 0, n = r.I32(); i < n; ++i) p.variables.push_back(ReadVariable(r, p.version));
        for (int32_t i = 0, n = r.I32(); i < n; ++i) ReadChannelProcessor(r);
        for (int32_t i = 0, n = r.I32(); i < n; ++i) ReadCbuffer(r);
        if (p.program == "features" || p.program == "vs")
            for (int32_t i = 0, n = r.I32(); i < n; ++i) ReadInputSignature(r);
        const int32_t n = r.I32();
        if (n < 0) throw Bad();
        std::vector<int64_t> ids((size_t)n);
        std::vector<int32_t> offs((size_t)n);
        for (int64_t& x : ids) x = r.I64();
        for (int32_t& x : offs) x = r.I32();
        if (n && (size_t)(uint32_t)r.I32() != data.size()) throw Bad();   // "end of file pointer"
        for (int32_t i = 0; i < n; ++i)
            p.combos.emplace_back(ids[i], ReadStaticCombo(DecompressCombo(data, offs[i], p.version, p.program), p.program,
                                                          p.version));
    } catch (const Bad&) {
        return false;
    }
    return true;
}

// --- The table.
struct LayoutField { std::string name; int32_t reg; uint16_t raw; int32_t type; };
struct LayoutRow { std::string shader, prog; std::vector<LayoutField> fields; };
struct Axis { std::string name, feature, states; };
struct AxisRow { std::string shader, prog; std::vector<Axis> axes; };
struct IndexRow { std::string chk; int lid; uint16_t fl; int aset; std::vector<int> vals; };

struct Tables {
    std::unordered_map<std::string, int> layoutIds;
    std::vector<LayoutRow> layouts;
    std::map<std::pair<std::string, std::string>, int> axisIds;
    std::vector<AxisRow> axes;
    std::vector<IndexRow> index;
};

int LayoutId(Tables& t, const std::string& shader, const std::string& prog, const Program& p, const IndexArray& seq) {
    LayoutRow row{ shader, prog, {} };
    std::string key = shader + '\0' + prog + '\0';
    for (const Field& f : seq.fields) {
        LayoutField lf;
        if (f.var < p.variables.size()) {
            const Variable& v = p.variables[f.var];
            lf = { v.name, v.reg, f.raw, v.type };
        } else {
            lf = { "?" + std::to_string(f.var), 0, f.raw, 0 };
        }
        key += lf.name;
        key += '\0';
        key.append((const char*)&lf.reg, 4);
        key.append((const char*)&lf.raw, 2);
        key.append((const char*)&lf.type, 4);
        row.fields.push_back(std::move(lf));
    }
    auto it = t.layoutIds.find(key);
    if (it != t.layoutIds.end()) return it->second;
    const int id = (int)t.layouts.size();
    t.layoutIds.emplace(std::move(key), id);
    t.layouts.push_back(std::move(row));
    return id;
}

int AxisSet(Tables& t, const std::string& shader, const std::string& prog, const Program& p, const Program* feat) {
    auto key = std::make_pair(shader, prog);
    auto it = t.axisIds.find(key);
    if (it != t.axisIds.end()) return it->second;
    AxisRow row{ shader, prog, {} };
    for (const Combo& c : p.statics) {
        Axis a{ c.name, "", "" };
        if (feat && c.feature >= 0 && (size_t)c.feature < feat->statics.size()) {
            const Combo& fc = feat->statics[c.feature];
            a.feature = fc.name;
            for (size_t i = 0; i < fc.states.size(); ++i) a.states += (i ? "|" : "") + fc.states[i];
        }
        row.axes.push_back(std::move(a));
    }
    const int id = (int)t.axes.size();
    t.axisIds.emplace(key, id);
    t.axes.push_back(std::move(row));
    return id;
}

std::vector<int> DecodeStatic(const Program& p, int64_t cid) {
    std::vector<int> vals;
    uint64_t off = 1;
    bool beyond = false;   // the radix has passed any id there is: every axis after is at its minimum
    for (const Combo& c : p.statics) {
        const int64_t n = (int64_t)c.max - c.min + 1;
        const int64_t q = beyond || n <= 0 ? 0 : (int64_t)((uint64_t)cid / off);
        vals.push_back((int)(n > 0 ? q % n : 0) + c.min);
        if (!beyond && n > 0) {
            if (off > UINT64_MAX / (uint64_t)n) beyond = true;
            else off *= (uint64_t)n;
        }
    }
    return vals;
}

uint16_t StateFlags(const State& st) {
    uint32_t fl = 0;
    if (st.hasBlend) {
        fl |= (st.a2c ? 1u : 0u) | ((st.enable0 ? 1u : 0u) << 1);
        fl |= (uint32_t)(st.src0 & 31) << 4 | (uint32_t)(st.dst0 & 31) << 9;
    } else {
        fl |= 1u << 14;
    }
    if (st.hasDepth) fl |= ((st.depthWrite ? 1u : 0u) << 2) | ((st.depthTest ? 1u : 0u) << 3);
    return (uint16_t)fl;
}

void IndexShader(Tables& t, const std::string& name, const std::map<std::string, Program>& programs) {
    auto fi = programs.find("features");
    const Program* feat = fi != programs.end() ? &fi->second : nullptr;
    for (const char* prog : { "vs", "ps", "cs", "gs", "hs", "ds", "psrs" }) {
        auto pi = programs.find(prog);
        if (pi == programs.end()) continue;
        const Program& p = pi->second;
        const int aset = AxisSet(t, name, prog, p, feat);
        const bool isPs = strcmp(prog, "ps") == 0 || strcmp(prog, "psrs") == 0;
        for (const auto& [cid, sc] : p.combos) {
            std::set<int32_t> seenFiles;
            const std::vector<int> vals = DecodeStatic(p, cid);
            for (const State& st : sc.states) {
                const int32_t f = st.file;
                if (f < 0 || (size_t)f >= sc.checksums.size() || sc.checksums[f].empty()) continue;
                const int64_t d = st.dyn;
                const IndexArray& seq = d >= 0 && (size_t)d < sc.dynamic.size() && !sc.dynamic[d].fields.empty()
                                        ? sc.dynamic[d] : sc.all;
                const int lid = LayoutId(t, name, prog, p, seq);
                const uint16_t fl = isPs ? StateFlags(st) : 0;
                if (seenFiles.insert(f).second) t.index.push_back({ sc.checksums[f], lid, fl, aset, vals });
            }
        }
    }
}

struct Writer {
    std::vector<uint8_t> b;
    void U8(uint32_t v) { b.push_back((uint8_t)v); }
    void U16(uint32_t v) { U8(v & 0xFF); U8((v >> 8) & 0xFF); }
    void U32(uint32_t v) { U16(v & 0xFFFF); U16(v >> 16); }
    void Bytes(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};

std::vector<uint8_t> WriteTables(const Tables& t, int* entries) {
    struct First { int lid; uint16_t fl; int aset; std::vector<int> vals; };
    std::unordered_map<std::string, size_t> firstAt;
    std::vector<std::pair<std::string, First>> first;   // in the order each checksum was first seen
    for (const IndexRow& row : t.index) {
        auto it = firstAt.find(row.chk);
        if (it == firstAt.end()) {
            firstAt.emplace(row.chk, first.size());
            first.push_back({ row.chk, { row.lid, row.fl, row.aset, row.vals } });
            continue;
        }
        First& had = first[it->second].second;
        if (had.aset == row.aset)
            for (size_t i = 0; i < row.vals.size() && i < had.vals.size(); ++i)
                if (had.vals[i] != row.vals[i]) had.vals[i] = 255;
    }
    std::unordered_map<std::string, uint32_t> strings;
    std::vector<const std::string*> stringOrder;
    auto sid = [&](const std::string& s) -> uint32_t {
        if (s.empty()) return 0xFFFF;
        auto it = strings.find(s);
        if (it != strings.end()) return it->second;
        const uint32_t i = (uint32_t)strings.size();
        auto res = strings.emplace(s, i);
        stringOrder.push_back(&res.first->first);
        return i;
    };
    auto stage = [](const std::string& p) -> uint32_t {
        static const std::map<std::string, uint32_t> m = { {"vs", 1}, {"ps", 2}, {"gs", 3}, {"hs", 4}, {"ds", 5},
                                                          {"cs", 6}, {"psrs", 7} };
        auto it = m.find(p);
        return it == m.end() ? 0 : it->second;
    };

    std::vector<uint32_t> layOffs, axOffs, coOffs;
    Writer lay, ax, co;
    for (const LayoutRow& row : t.layouts) {
        layOffs.push_back((uint32_t)lay.b.size());
        lay.U16(sid(row.shader));
        lay.U8(stage(row.prog));
        lay.U16((uint32_t)row.fields.size());
        for (const LayoutField& f : row.fields) {
            lay.U16(sid(f.name));
            lay.U8((uint32_t)f.reg & 0xFF);
            lay.U16(f.raw);
            lay.U8((uint32_t)f.type & 0xFF);
        }
    }
    for (const AxisRow& row : t.axes) {
        axOffs.push_back((uint32_t)ax.b.size());
        ax.U16(sid(row.shader));
        ax.U8(stage(row.prog));
        ax.U16((uint32_t)row.axes.size());
        for (const Axis& a : row.axes) {
            ax.U16(sid(a.name));
            ax.U16(sid(a.feature));
            ax.U16(sid(a.states));
        }
    }
    std::map<std::pair<int, std::vector<int>>, uint32_t> combos;
    std::vector<std::pair<int, std::vector<int>>> comboRows;
    std::unordered_map<std::string, uint32_t> entryCombo;
    for (const auto& [chk, f] : first) {
        std::vector<int> key = f.vals;
        for (int& v : key) v = (std::min)(v, 255);
        auto k = std::make_pair(f.aset, key);
        auto it = combos.find(k);
        uint32_t ci;
        if (it == combos.end()) {
            ci = (uint32_t)comboRows.size();
            combos.emplace(k, ci);
            comboRows.push_back(k);
        } else {
            ci = it->second;
        }
        entryCombo[chk] = ci;
    }
    for (const auto& [aset, vals] : comboRows) {
        coOffs.push_back((uint32_t)co.b.size());
        co.U16((uint32_t)aset);
        co.U16((uint32_t)vals.size());
        for (int v : vals) co.U8((uint32_t)v);
    }
    Writer out;
    out.Bytes("DXRS", 4);
    out.U32(3);
    out.U32((uint32_t)strings.size());
    out.U32((uint32_t)t.layouts.size());
    out.U32((uint32_t)t.axes.size());
    out.U32((uint32_t)comboRows.size());
    out.U32((uint32_t)first.size());
    for (const std::string* s : stringOrder) {
        out.U16((uint32_t)s->size());
        out.Bytes(s->data(), s->size());
    }
    const std::pair<const std::vector<uint32_t>*, const Writer*> blocks[3] = { {&layOffs, &lay}, {&axOffs, &ax},
                                                                               {&coOffs, &co} };
    for (const auto& [offs, data] : blocks) {
        for (uint32_t o : *offs) out.U32(o);
        out.U32((uint32_t)data->b.size());
        out.Bytes(data->b.data(), data->b.size());
    }
    std::vector<const std::pair<std::string, First>*> sorted;
    for (const auto& e : first) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
        return memcmp(a->first.data(), b->first.data(), 16) < 0;
    });
    for (const auto* e : sorted) {
        out.Bytes(e->first.data(), 16);
        out.U32((uint32_t)e->second.lid);
        out.U32(entryCombo[e->first]);
        out.U16(e->second.fl);
        out.U16(0);
    }
    *entries = (int)first.size();
    return out.b;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? (char)c : '?';
    return s;
}

}  // namespace

bool BuildShaderTable(const std::wstring& gameDir, const std::wstring& outPath, ShaderTableReport* report) {
    const auto start = std::chrono::steady_clock::now();
    ShaderTableReport rep;
    std::vector<std::string> order;
    std::unordered_map<std::string, Entry> entries;
    for (const wchar_t* sub : { L"\\core\\shaders_pc_dir.vpk", L"\\hlvr\\shaders_pc_dir.vpk" }) {
        if (!VpkEntries(gameDir + sub, order, entries)) {
            rep.firstProblems = "could not read " + Narrow(gameDir + sub) + "\n";
            if (report) *report = rep;
            return false;
        }
    }
    // shader -> program -> file name; a later version of the same program (pc_41 after pc_50, say) takes its place,
    // in the archives' own order, as in the Python.
    std::map<std::string, std::map<std::string, std::string>> shaders;
    for (const std::string& fname : order) {
        if (fname.size() < 4 || fname.compare(fname.size() - 4, 4, ".vcs") != 0) continue;
        const std::string base = fname.substr(0, fname.size() - 4);
        std::vector<std::string> parts;
        for (size_t s = 0;;) {
            const size_t e = base.find('_', s);
            parts.push_back(base.substr(s, e == std::string::npos ? std::string::npos : e - s));
            if (e == std::string::npos) break;
            s = e + 1;
        }
        if (parts.size() < 4 || parts[parts.size() - 3] != "pc") continue;
        const std::string& ver = parts[parts.size() - 2];
        if (ver != "50" && ver != "41" && ver != "40") continue;
        std::string shader;
        for (size_t i = 0; i + 3 < parts.size(); ++i) shader += (i ? "_" : "") + parts[i];
        shaders[shader][parts.back()] = fname;
    }
    Tables t;
    std::vector<uint8_t> data;
    for (const auto& [shader, progs] : shaders) {
        std::map<std::string, Program> programs;
        for (const auto& [prog, fname] : progs) {
            // Only what the table is made from: the features (the checkboxes' names) and the stages it indexes.
            if (prog != "features" && prog != "vs" && prog != "ps" && prog != "cs" && prog != "gs" && prog != "hs"
                && prog != "ds" && prog != "psrs") continue;
            Program p;
            if (VpkRead(entries[fname], data) && ReadProgram(data, fname, p)) {
                programs.emplace(prog, std::move(p));
                ++rep.programs;
            } else {
                if (++rep.problems <= 20) rep.firstProblems += fname + "\n";
            }
        }
        IndexShader(t, shader, programs);
        ++rep.shaders;
    }
    rep.files = (int)t.index.size();
    rep.layouts = (int)t.layouts.size();
    const std::vector<uint8_t> table = WriteTables(t, &rep.entries);
    const std::wstring tmp = outPath + L".part";
    FILE* f = nullptr;
    bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
    if (ok) {
        ok = fwrite(table.data(), 1, table.size(), f) == table.size();
        ok = fclose(f) == 0 && ok;
    }
    ok = ok && MoveFileExW(tmp.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (report) *report = rep;
    return ok;
}
