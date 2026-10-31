#include "vcs.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdlib>
#include <thread>

#include "LzmaDec.h"
#include "LzmaEnc.h"

namespace vcs
{
namespace
{
constexpr uint32_t MAGIC = 0x32736376;          // "vcs2"
constexpr uint32_t LZMA_MAGIC = 0x414D5A4C;     // "LZMA"
constexpr int32_t SBMS = 0x534D4253;            // a variable's UI visibility expression follows

void* LzAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void LzFree(ISzAllocPtr, void* p) { std::free(p); }
const ISzAlloc LZ_ALLOC = {LzAlloc, LzFree};

int ProgramCount(int version, uint32_t flags)
{
    int count = 7;
    if (version >= 68)
        count -= 2;
    if (version < 63)
        count -= 1;
    if (flags & 4)
        count += 3;
    else if (flags & 2)
        count += 2;
    else if (flags & 1)
        count += 1;
    return count;
}

void Check(bool ok, const char* what)
{
    if (!ok)
        throw FormatError(what);
}

// ------------------------------------------------------------------------------------------- header blocks
Combo ReadCombo(Reader& r)
{
    Combo c;
    c.name = r.raw(64);
    c.alias = r.raw(64);
    c.type = r.take<int32_t>();
    c.min = r.take<int32_t>();
    c.max = r.take<int32_t>();
    c.source = r.take<int32_t>();
    c.feature = r.take<int32_t>();
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        c.states.push_back(r.cstr());
    if (c.source == 10 || c.source == 11)
        c.compare = r.take<int32_t>();
    return c;
}

void WriteCombo(Writer& w, const Combo& c)
{
    w.raw(c.name);
    w.raw(c.alias);
    w.put(c.type);
    w.put(c.min);
    w.put(c.max);
    w.put(c.source);
    w.put(c.feature);
    w.put(static_cast<int32_t>(c.states.size()));
    for (const auto& s : c.states)
        w.cstr(s);
    if (c.source == 10 || c.source == 11)
        w.put(c.compare);
}

template <typename T, size_t N> void TakeArray(Reader& r, std::array<T, N>& a)
{
    for (auto& x : a)
        x = r.take<T>();
}

template <typename T, size_t N> void PutArray(Writer& w, const std::array<T, N>& a)
{
    for (auto x : a)
        w.put(x);
}

Rule ReadRule(Reader& r)
{
    Rule u;
    u.method = r.take<int32_t>();
    u.type = r.take<int32_t>();
    TakeArray(r, u.argTypes);
    TakeArray(r, u.argIndices);
    TakeArray(r, u.argValues);
    TakeArray(r, u.extra);
    u.error = r.raw(256);
    return u;
}

void WriteRule(Writer& w, const Rule& u)
{
    w.put(u.method);
    w.put(u.type);
    PutArray(w, u.argTypes);
    PutArray(w, u.argIndices);
    PutArray(w, u.argValues);
    PutArray(w, u.extra);
    w.raw(u.error);
}

Variable ReadVariable(Reader& r)
{
    Variable v;
    v.name = r.raw(64);
    v.group = r.raw(64);
    v.ui = r.take<int32_t>();
    v.step = r.take<float>();
    v.sourceString = r.raw(64);
    v.source = r.take<int32_t>();
    if (v.source == 6 || v.source == 7)
        v.expression = r.raw(r.take<int32_t>());
    v.sourceIndex = r.take<int32_t>();
    if (v.sourceIndex == SBMS)
    {
        v.visibility = r.raw(r.take<int32_t>());
        v.sourceIndex = r.take<int32_t>();
    }
    v.type = r.take<int32_t>();
    v.reg = r.take<int32_t>();
    v.context = r.take<int32_t>();
    v.elements = r.take<int32_t>();
    v.bits = r.take<int32_t>();
    v.defaultTexture = r.raw(64);
    TakeArray(r, v.intDefs);
    TakeArray(r, v.intMins);
    TakeArray(r, v.intMaxs);
    TakeArray(r, v.floatDefs);
    TakeArray(r, v.floatMins);
    TakeArray(r, v.floatMaxs);
    v.format = r.take<int32_t>();
    v.channels = r.take<int32_t>();
    TakeArray(r, v.channelInfo);
    v.colorSpace = r.take<int32_t>();
    v.precision = r.take<int32_t>();
    v.fileEnding = r.raw(32);
    v.processing = r.raw(32);
    return v;
}

void WriteVariable(Writer& w, const Variable& v)
{
    w.raw(v.name);
    w.raw(v.group);
    w.put(v.ui);
    w.put(v.step);
    w.raw(v.sourceString);
    w.put(v.source);
    if (v.source == 6 || v.source == 7)
    {
        w.put(static_cast<int32_t>(v.expression.size()));
        w.raw(v.expression);
    }
    if (v.visibility)
    {
        w.put(SBMS);
        w.put(static_cast<int32_t>(v.visibility->size()));
        w.raw(*v.visibility);
    }
    w.put(v.sourceIndex);
    w.put(v.type);
    w.put(v.reg);
    w.put(v.context);
    w.put(v.elements);
    w.put(v.bits);
    w.raw(v.defaultTexture);
    PutArray(w, v.intDefs);
    PutArray(w, v.intMins);
    PutArray(w, v.intMaxs);
    PutArray(w, v.floatDefs);
    PutArray(w, v.floatMins);
    PutArray(w, v.floatMaxs);
    w.put(v.format);
    w.put(v.channels);
    PutArray(w, v.channelInfo);
    w.put(v.colorSpace);
    w.put(v.precision);
    w.raw(v.fileEnding);
    w.raw(v.processing);
}

Channel ReadChannel(Reader& r)
{
    Channel c;
    c.channel = r.take<uint32_t>();
    TakeArray(r, c.inputs);
    c.colorSpace = r.take<int32_t>();
    c.command = r.raw(256);
    return c;
}

void WriteChannel(Writer& w, const Channel& c)
{
    w.put(c.channel);
    PutArray(w, c.inputs);
    w.put(c.colorSpace);
    w.raw(c.command);
}

CBuffer ReadCBuffer(Reader& r)
{
    CBuffer cb;
    cb.name = r.raw(64);
    cb.size = r.take<uint32_t>();
    cb.type = r.take<int32_t>();
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
    {
        CBufferVar v;
        v.name = r.raw(64);
        v.offset = r.take<int32_t>();
        v.vector = r.take<int32_t>();
        v.rows = r.take<int32_t>();
        v.elements = r.take<int32_t>();
        cb.vars.push_back(v);
    }
    cb.crc = r.take<uint32_t>();
    return cb;
}

void WriteCBuffer(Writer& w, const CBuffer& cb)
{
    w.raw(cb.name);
    w.put(cb.size);
    w.put(cb.type);
    w.put(static_cast<int32_t>(cb.vars.size()));
    for (const auto& v : cb.vars)
    {
        w.raw(v.name);
        w.put(v.offset);
        w.put(v.vector);
        w.put(v.rows);
        w.put(v.elements);
    }
    w.put(cb.crc);
}

Signature ReadSignature(Reader& r)
{
    Signature sig;
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
    {
        SignatureEntry e;
        e.name = r.cstr();
        e.d3dSemantic = r.cstr();
        e.semantic = r.cstr();
        e.index = r.take<int32_t>();
        sig.push_back(e);
    }
    return sig;
}

void WriteSignature(Writer& w, const Signature& sig)
{
    w.put(static_cast<int32_t>(sig.size()));
    for (const auto& e : sig)
    {
        w.cstr(e.name);
        w.cstr(e.d3dSemantic);
        w.cstr(e.semantic);
        w.put(e.index);
    }
}

// ------------------------------------------------------------------------------------------- static combos
int32_t ReadCount(Reader& r)
{
    uint16_t n = r.take<uint16_t>();
    return n == 0xFFFF ? r.take<int32_t>() : n;
}

void WriteCount(Writer& w, size_t n)
{
    if (n >= 0xFFFF)
    {
        w.put(static_cast<uint16_t>(0xFFFF));
        w.put(static_cast<int32_t>(n));
    }
    else
        w.put(static_cast<uint16_t>(n));
}

IndexArray ReadIndexArray(Reader& r, bool offsets)
{
    IndexArray a;
    int32_t count = r.take<int32_t>();
    a.firstRenderState = r.take<int32_t>();
    a.firstConstant = r.take<int32_t>();
    for (int32_t i = 0; i < count; i++)
    {
        Field f;
        f.var = r.take<uint16_t>();
        if (offsets)
            f.slot = r.take<uint16_t>();
        a.fields.push_back(f);
    }
    return a;
}

void WriteIndexArray(Writer& w, const IndexArray& a, bool offsets)
{
    w.put(static_cast<int32_t>(a.fields.size()));
    w.put(a.firstRenderState);
    w.put(a.firstConstant);
    for (const auto& f : a.fields)
    {
        w.put(f.var);
        if (offsets)
            w.put(f.slot);
    }
}

Attribute ReadAttribute(Reader& r)
{
    Attribute a;
    a.name = r.cstr();
    a.hash = r.take<uint32_t>();
    a.type = r.take<uint8_t>();
    a.binding = r.take<int16_t>();
    if (a.binding != -1)
        return a;
    int32_t n = r.take<int32_t>();
    if (n > 0)
    {
        a.expression = r.raw(n);
        return a;
    }
    switch (a.type)
    {
    case 1: a.f = r.take<float>(); break;
    case 5: a.i = r.take<int32_t>(); break;
    case 9: a.b = r.take<uint8_t>(); break;
    case 41: a.s = r.cstr(); break;
    case 2:
    case 3:
    case 4:
        for (int k = 0; k < a.type; k++)
            a.v.push_back(r.take<float>());
        break;
    default: throw FormatError("attribute of an unknown type with a constant");
    }
    return a;
}

void WriteAttribute(Writer& w, const Attribute& a)
{
    w.cstr(a.name);
    w.put(a.hash);
    w.put(a.type);
    w.put(a.binding);
    if (a.binding != -1)
        return;
    if (a.expression)
    {
        w.put(static_cast<int32_t>(a.expression->size()));
        w.raw(*a.expression);
        return;
    }
    w.put(static_cast<int32_t>(0));
    switch (a.type)
    {
    case 1: w.put(a.f); break;
    case 5: w.put(a.i); break;
    case 9: w.put(a.b); break;
    case 41: w.cstr(a.s); break;
    default:
        for (float x : a.v)
            w.put(x);
    }
}

// ------------------------------------------------------------------------------------------- blocks on disk
Bytes LzmaDecode(const Bytes& props, const Bytes& body, uint32_t size)
{
    Bytes out(size);
    SizeT outLen = size, inLen = body.size();
    ELzmaStatus status;
    SRes res = ::LzmaDecode(out.data(), &outLen, body.data(), &inLen, props.data(), 5, LZMA_FINISH_ANY, &status,
                            &LZ_ALLOC);
    Check(res == SZ_OK && outLen == size, "a block's LZMA data does not decode");
    return out;
}

std::pair<Bytes, Bytes> UnpackBlock(const Bytes& data, size_t off)
{
    Reader r(data, off);
    int32_t first = r.take<int32_t>();
    if (first >= 0)
    {
        if (r.take<uint32_t>() != LZMA_MAGIC)
            return {Bytes(data.begin() + off + 4, data.begin() + off + 4 + first),
                    Bytes(data.begin() + off, data.begin() + off + 4 + first)};
        uint32_t size = r.take<uint32_t>();
        uint32_t csize = r.take<uint32_t>();
        Bytes props = r.raw(5);
        Bytes body = r.raw(csize);
        return {LzmaDecode(props, body, size), Bytes(data.begin() + off, data.begin() + r.pos())};
    }
    int32_t size = r.take<int32_t>();
    int32_t csize = r.take<int32_t>();
    Check(first == -1 && size == csize, "an unknown block packing");
    return {Bytes(data.begin() + r.pos(), data.begin() + r.pos() + size),
            Bytes(data.begin() + off, data.begin() + r.pos() + size)};
}
} // namespace

Bytes Fixed(const std::string& s, size_t n)
{
    if (s.size() >= n)
        throw FormatError("text too long for its field: " + s);
    Bytes b(s.begin(), s.end());
    b.resize(n, 0);
    return b;
}

std::string Text(const Bytes& field)
{
    size_t e = 0;
    while (e < field.size() && field[e] != 0)
        e++;
    return std::string(field.begin(), field.begin() + e);
}

uint32_t Murmur2(const std::string& name, uint32_t seed)
{
    std::string data = name;
    for (auto& c : data)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    const uint32_t m = 0x5BD1E995;
    const int r = 24;
    uint32_t h = seed ^ static_cast<uint32_t>(data.size());
    size_t n = data.size() / 4;
    for (size_t i = 0; i < n; i++)
    {
        uint32_t k;
        std::memcpy(&k, data.data() + i * 4, 4);
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
    }
    const auto* rest = reinterpret_cast<const uint8_t*>(data.data()) + n * 4;
    switch (data.size() & 3)
    {
    case 3: h ^= rest[2] << 16; [[fallthrough]];
    case 2: h ^= rest[1] << 8; [[fallthrough]];
    case 1:
        h ^= rest[0];
        h *= m;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

Bytes Md5(const Bytes& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    Bytes out(16);
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0 &&
              BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), out.data(),
                         16) == 0;
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    Check(ok, "MD5 failed");
    return out;
}

StaticCombo ReadStaticCombo(const Bytes& blob, const std::string& program)
{
    Reader r(blob);
    StaticCombo sc;
    sc.all = ReadIndexArray(r, program != "features");
    for (int16_t n = r.take<int16_t>(); n > 0; n--)
        sc.attributes.push_back(ReadAttribute(r));
    if (program == "features" || program == "vs")
    {
        for (int32_t n = ReadCount(r); n > 0; n--)
            sc.vsInputs.push_back(r.take<int16_t>());
        if (program == "features")
        {
            Check(r.pos() == blob.size(), "features static combo not read to its end");
            return sc;
        }
    }
    for (int32_t n = ReadCount(r); n > 0; n--)
        sc.dynamic.push_back(ReadIndexArray(r, true));
    for (int32_t n = ReadCount(r); n > 0; n--)
    {
        uint8_t a = r.take<uint8_t>();
        uint8_t b = r.take<uint8_t>();
        sc.cbSlots.emplace_back(a, b);
    }
    sc.cbSize = r.take<int32_t>();
    sc.staticCB = r.take<uint8_t>();
    int32_t nfiles = r.take<int32_t>();
    sc.glsl = r.take<uint8_t>();
    for (int32_t i = 0; i < nfiles; i++)
    {
        CodeFile f;
        int32_t size = r.take<int32_t>();
        if (size > 0)
            f.data = std::make_shared<const Bytes>(r.raw(size));
        f.hash = r.raw(16);
        sc.files.push_back(f);
    }
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
    {
        State st;
        st.dyn = r.take<int64_t>();
        st.file = r.take<int32_t>();
        st.fileOffset = r.take<int32_t>();
        if (program == "ps" || program == "psrs")
        {
            for (auto& h : st.has)
                h = r.take<uint8_t>();
            for (int i = 0; i < 3; i++)
                if (st.has[i] == 0)
                    st.rs[i] = r.raw(RS_SIZES[i]);
        }
        else if (program == "hs")
            st.hsArg = r.take<uint8_t>();
        sc.states.push_back(st);
    }
    Check(r.pos() == blob.size(), "static combo not read to its end");
    return sc;
}

Bytes WriteStaticCombo(const StaticCombo& sc, const std::string& program)
{
    Writer w;
    WriteIndexArray(w, sc.all, program != "features");
    w.put(static_cast<int16_t>(sc.attributes.size()));
    for (const auto& a : sc.attributes)
        WriteAttribute(w, a);
    if (program == "features" || program == "vs")
    {
        WriteCount(w, sc.vsInputs.size());
        for (auto i : sc.vsInputs)
            w.put(i);
        if (program == "features")
            return w.bytes();
    }
    WriteCount(w, sc.dynamic.size());
    for (const auto& a : sc.dynamic)
        WriteIndexArray(w, a, true);
    WriteCount(w, sc.cbSlots.size());
    for (const auto& s : sc.cbSlots)
    {
        w.put(s.first);
        w.put(s.second);
    }
    w.put(sc.cbSize);
    w.put(sc.staticCB);
    w.put(static_cast<int32_t>(sc.files.size()));
    w.put(sc.glsl);
    for (const auto& f : sc.files)
    {
        w.put(static_cast<int32_t>(f.code().size()));
        w.raw(f.code());
        w.raw(f.hash);
    }
    w.put(static_cast<int32_t>(sc.states.size()));
    for (const auto& st : sc.states)
    {
        w.put(st.dyn);
        w.put(st.file);
        w.put(st.fileOffset);
        if (program == "ps" || program == "psrs")
        {
            for (auto h : st.has)
                w.put(h);
            for (int i = 0; i < 3; i++)
                if (st.has[i] == 0)
                    w.raw(st.rs[i]);
        }
        else if (program == "hs")
            w.put(st.hsArg);
    }
    return w.bytes();
}

namespace
{
int g_lzmaLevel = 9;
}

void SetLzmaLevel(int level)
{
    g_lzmaLevel = level < 0 ? 0 : level > 9 ? 9 : level;
}

Bytes PackBlock(const Bytes& blob, Pack how)
{
    Writer w;
    if (how == Pack::Raw)
    {
        w.put(static_cast<int32_t>(blob.size()));
        w.raw(blob);
        return w.bytes();
    }
    if (how == Pack::Stored)
    {
        w.put(static_cast<int32_t>(-1));
        w.put(static_cast<int32_t>(blob.size()));
        w.put(static_cast<int32_t>(blob.size()));
        w.raw(blob);
        return w.bytes();
    }
    // LZMA as the game's files have it: a 1 MB dictionary, lc 3, lp 0, pb 2, no end marker; level 9 unless set lower.
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    props.level = g_lzmaLevel;
    props.dictSize = 1 << 20;
    props.lc = 3;
    props.lp = 0;
    props.pb = 2;
    props.numThreads = 1;
    Bytes body(blob.size() + blob.size() / 3 + 1024);
    SizeT bodyLen = body.size();
    Bytes encodedProps(5);
    SizeT propsLen = 5;
    SRes res = LzmaEncode(body.data(), &bodyLen, blob.data(), blob.size(), &props, encodedProps.data(), &propsLen, 0,
                          nullptr, &LZ_ALLOC, &LZ_ALLOC);
    Check(res == SZ_OK && propsLen == 5, "LZMA compression failed");
    body.resize(bodyLen);
    // The block's first int is the size of what follows it: the magic, the two sizes, props and data.
    w.put(static_cast<int32_t>(12 + 5 + body.size()));
    w.put(LZMA_MAGIC);
    w.put(static_cast<uint32_t>(blob.size()));
    w.put(static_cast<uint32_t>(body.size()));
    w.raw(encodedProps);
    w.raw(body);
    return w.bytes();
}

File Read(const Bytes& data, const std::string& program)
{
    Reader r(data);
    Check(r.take<uint32_t>() == MAGIC, "not a vcs2 file");
    File f;
    f.program = program;
    f.version = r.take<int32_t>();
    f.flags = r.take<uint32_t>();
    Check(f.version == 64, "not version 64");
    int count = ProgramCount(f.version, f.flags);
    if (program == "features")
    {
        f.headerVersion = r.take<int32_t>();
        int32_t length = r.take<int32_t>();
        f.description = r.cstr();
        Check(length == static_cast<int32_t>(f.description.size()), "description length");
        f.dev = r.take<int32_t>();
        for (int i = 0; i < count; i++)
            f.available.push_back(r.take<int32_t>());
        for (int32_t n = r.take<int32_t>(); n > 0; n--)
        {
            Mode m;
            m.name = r.raw(64);
            m.fallback = r.raw(64);
            for (int32_t k = r.take<int32_t>(); k > 0; k--)
            {
                Bytes name = r.raw(64);
                m.combos.emplace_back(name, r.take<int32_t>());
            }
            f.modes.push_back(m);
        }
        for (int i = 0; i < count; i++)
            f.programHashes.push_back(r.raw(16));
    }
    else
        f.programHashes.push_back(r.raw(16));
    f.variableHash = r.raw(16);
    f.variableSourceMax = r.take<int32_t>();
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        f.statics.push_back(ReadCombo(r));
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        f.staticRules.push_back(ReadRule(r));
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        f.dynamics.push_back(ReadCombo(r));
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        f.dynamicRules.push_back(ReadRule(r));
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        f.variables.push_back(ReadVariable(r));
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        f.channels.push_back(ReadChannel(r));
    for (int32_t n = r.take<int32_t>(); n > 0; n--)
        f.cbuffers.push_back(ReadCBuffer(r));
    if (program == "features" || program == "vs")
        for (int32_t n = r.take<int32_t>(); n > 0; n--)
            f.signatures.push_back(ReadSignature(r));
    int32_t n = r.take<int32_t>();
    std::vector<int64_t> ids;
    std::vector<int32_t> offs;
    for (int32_t i = 0; i < n; i++)
        ids.push_back(r.take<int64_t>());
    for (int32_t i = 0; i < n; i++)
        offs.push_back(r.take<int32_t>());
    if (n)
    {
        Check(r.take<int32_t>() == static_cast<int32_t>(data.size()), "file size");
        Check(offs[0] == static_cast<int32_t>(r.pos()), "first block offset");
        for (int32_t i = 0; i < n; i++)
        {
            Block b;
            b.id = ids[i];
            auto [body, stored] = UnpackBlock(data, offs[i]);
            b.data = std::move(body);
            b.stored = std::move(stored);
            f.blocks.push_back(std::move(b));
        }
    }
    else
        Check(r.pos() == data.size(), "file not read to its end");
    return f;
}

Bytes Write(const File& f, Pack pack)
{
    Writer w;
    w.put(MAGIC);
    w.put(f.version);
    w.put(f.flags);
    if (f.program == "features")
    {
        w.put(f.headerVersion);
        w.put(static_cast<int32_t>(f.description.size()));
        w.cstr(f.description);
        w.put(f.dev);
        for (auto a : f.available)
            w.put(a);
        w.put(static_cast<int32_t>(f.modes.size()));
        for (const auto& m : f.modes)
        {
            w.raw(m.name);
            w.raw(m.fallback);
            w.put(static_cast<int32_t>(m.combos.size()));
            for (const auto& [name, value] : m.combos)
            {
                w.raw(name);
                w.put(value);
            }
        }
    }
    for (const auto& h : f.programHashes)
        w.raw(h);
    w.raw(f.variableHash);
    w.put(f.variableSourceMax);
    w.put(static_cast<int32_t>(f.statics.size()));
    for (const auto& c : f.statics)
        WriteCombo(w, c);
    w.put(static_cast<int32_t>(f.staticRules.size()));
    for (const auto& u : f.staticRules)
        WriteRule(w, u);
    w.put(static_cast<int32_t>(f.dynamics.size()));
    for (const auto& c : f.dynamics)
        WriteCombo(w, c);
    w.put(static_cast<int32_t>(f.dynamicRules.size()));
    for (const auto& u : f.dynamicRules)
        WriteRule(w, u);
    w.put(static_cast<int32_t>(f.variables.size()));
    for (const auto& v : f.variables)
        WriteVariable(w, v);
    w.put(static_cast<int32_t>(f.channels.size()));
    for (const auto& c : f.channels)
        WriteChannel(w, c);
    w.put(static_cast<int32_t>(f.cbuffers.size()));
    for (const auto& cb : f.cbuffers)
        WriteCBuffer(w, cb);
    if (f.program == "features" || f.program == "vs")
    {
        w.put(static_cast<int32_t>(f.signatures.size()));
        for (const auto& s : f.signatures)
            WriteSignature(w, s);
    }
    w.put(static_cast<int32_t>(f.blocks.size()));
    if (f.blocks.empty())
        return w.bytes();
    for (const auto& b : f.blocks)
        w.put(b.id);
    // Each block on its own, so on all cores: a lit shader has thousands, and LZMA is most of the writing time.
    std::vector<Bytes> stored(f.blocks.size());
    std::atomic<size_t> next{0};
    auto work = [&]() {
        for (size_t i; (i = next++) < f.blocks.size();)
            stored[i] = pack == Pack::Keep ? f.blocks[i].stored : PackBlock(f.blocks[i].data, pack);
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < std::max(1u, std::thread::hardware_concurrency()); t++)
        threads.emplace_back(work);
    work();
    for (auto& t : threads)
        t.join();
    size_t pos = w.size() + 4 * f.blocks.size() + 4;
    for (const auto& s : stored)
    {
        w.put(static_cast<int32_t>(pos));
        pos += s.size();
    }
    w.put(static_cast<int32_t>(pos));
    for (const auto& s : stored)
        w.raw(s);
    return w.bytes();
}

} // namespace vcs
