#include "hlsl.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>

namespace hlsl
{
namespace
{
std::string BlobText(ID3DBlob* blob)
{
    if (!blob)
        return {};
    return std::string(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
}

uint32_t U32(const Bytes& b, size_t off)
{
    if (off + 4 > b.size())
        throw FormatError("DXBC read past the end");
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

std::string Str(const Bytes& b, size_t off)
{
    size_t e = off;
    while (e < b.size() && b[e] != 0)
        e++;
    return std::string(b.begin() + off, b.begin() + e);
}

// The body of a chunk of a DXBC container, or nothing.
bool Chunk(const Bytes& code, const char* fourcc, Bytes& out)
{
    if (code.size() < 32 || std::memcmp(code.data(), "DXBC", 4) != 0)
        throw FormatError("not DXBC");
    uint32_t n = U32(code, 28);
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t off = U32(code, 32 + 4 * i);
        if (std::memcmp(code.data() + off, fourcc, 4) == 0)
        {
            uint32_t len = U32(code, off + 4);
            out.assign(code.begin() + off + 8, code.begin() + off + 8 + len);
            return true;
        }
    }
    return false;
}

const char* INPUT_TYPES[] = {"cbuffer", "tbuffer", "texture", "sampler", "uav", "structured", "uav_structured",
                             "byteaddress", "uav_byteaddress"};

pD3DCompile g_compile = nullptr;

// S2VFXC_FLAGS: D3DCompile flags as a number, for trying what makes a shader faster to build (default 0).
UINT CompileFlags()
{
    const char* f = std::getenv("S2VFXC_FLAGS");
    return f ? static_cast<UINT>(std::strtoul(f, nullptr, 0)) : 0;
}
} // namespace

void UseGameCompiler(const std::filesystem::path& game)
{
    HMODULE dll = LoadLibraryW((game / "bin" / "win64" / "d3dcompiler_43.dll").c_str());
    if (!dll)
        dll = LoadLibraryW(L"d3dcompiler_43.dll");
    if (!dll)
        throw CompileError("d3dcompiler_43.dll not found in the game's bin\\win64 or the system");
    g_compile = reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile"));
    if (!g_compile)
        throw CompileError("d3dcompiler_43.dll has no D3DCompile");
}

Bytes Compile(const std::string& source, const std::string& entry, const std::string& target, const Defines& defines,
              const std::string& name)
{
    if (!g_compile)
        throw CompileError("no HLSL compiler loaded (hlsl::UseGameCompiler)");
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& [k, v] : defines)
        macros.push_back({k.c_str(), v.c_str()});
    macros.push_back({nullptr, nullptr});
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = g_compile(source.data(), source.size(), name.c_str(), macros.data(), nullptr, entry.c_str(),
                           target.c_str(), CompileFlags(), 0, &code, &errors);
    std::string msg = BlobText(errors);
    if (errors)
        errors->Release();
    if (FAILED(hr) || !code)
    {
        if (code)
            code->Release();
        char head[32];
        std::snprintf(head, sizeof(head), "compile failed 0x%08lx\n", static_cast<unsigned long>(hr));
        throw CompileError(head + msg);
    }
    const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    Bytes out(p, p + code->GetBufferSize());
    code->Release();
    return out;
}

std::string Disassemble(const Bytes& code)
{
    ID3DBlob* text = nullptr;
    if (FAILED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &text)) || !text)
        return "D3DDisassemble failed";
    std::string s = BlobText(text);
    text->Release();
    return s;
}

std::vector<SignatureElement> Signature(const Bytes& code, const char* which)
{
    std::vector<SignatureElement> out;
    Bytes c;
    if (!Chunk(code, which, c))
        return out;
    uint32_t n = U32(c, 0);
    for (uint32_t k = 0; k < n; k++)
    {
        size_t e = 8 + 24 * k;
        SignatureElement s;
        s.semantic = Str(c, U32(c, e));
        s.index = U32(c, e + 4);
        s.sv = U32(c, e + 8);
        s.type = U32(c, e + 12);
        s.reg = U32(c, e + 16);
        s.mask = c[e + 20];
        s.used = c[e + 21];
        out.push_back(s);
    }
    return out;
}

const CBufferInfo* Reflection::Find(const std::string& name) const
{
    for (const auto& [n, cb] : cbuffers)
        if (n == name)
            return &cb;
    return nullptr;
}

Reflection Reflect(const Bytes& code)
{
    Reflection out;
    Bytes c;
    if (!Chunk(code, "RDEF", c))
        return out;
    uint32_t ncb = U32(c, 0), cboff = U32(c, 4), nres = U32(c, 8), resoff = U32(c, 12);
    uint8_t major = c[17];
    for (uint32_t i = 0; i < nres; i++)
    {
        size_t e = resoff + 32 * i;
        Resource r;
        r.name = Str(c, U32(c, e));
        uint32_t type = U32(c, e + 4);
        r.type = type < 9 ? INPUT_TYPES[type] : std::to_string(type);
        r.dimension = U32(c, e + 12);
        r.bind = U32(c, e + 20);
        r.count = U32(c, e + 24);
        out.resources.push_back(r);
    }
    uint32_t vstride = major >= 5 ? 40 : 24;
    for (uint32_t i = 0; i < ncb; i++)
    {
        size_t e = cboff + 24 * i;
        CBufferInfo cb;
        std::string name = Str(c, U32(c, e));
        uint32_t nvar = U32(c, e + 4), varoff = U32(c, e + 8);
        cb.size = U32(c, e + 12);
        for (uint32_t k = 0; k < nvar; k++)
        {
            size_t v = varoff + vstride * k;
            CBufferVar var;
            var.name = Str(c, U32(c, v));
            var.offset = U32(c, v + 4);
            var.size = U32(c, v + 8);
            var.used = (U32(c, v + 12) & 2) != 0;
            cb.vars.push_back(var);
        }
        out.cbuffers.emplace_back(name, cb);
    }
    return out;
}

} // namespace hlsl
