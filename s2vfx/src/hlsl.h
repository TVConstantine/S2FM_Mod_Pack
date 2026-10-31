// HLSL to DXBC with the game's own d3dcompiler_43.dll (the one Valve's shader compiler uses), and the small reads of
// DXBC the compiler needs: the signatures and the reflection chunk (RDEF) with its resources and constant buffers.
// Disassembly uses Windows' d3dcompiler_47.dll.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bytes.h"

namespace hlsl
{

struct CompileError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

using Defines = std::vector<std::pair<std::string, std::string>>;

// The HLSL compiler: the game's bin\win64\d3dcompiler_43.dll, else the system's. d3dcompiler_47 turns an integer read
// from a constant buffer array inside a loop of unknown length into a select over the whole array (the light list's
// switches: 566 instructions instead of 22), so Valve's compiler is the one that gives Valve's code.
void UseGameCompiler(const std::filesystem::path& game);

Bytes Compile(const std::string& source, const std::string& entry, const std::string& target, const Defines& defines,
              const std::string& name);
std::string Disassemble(const Bytes& code);

struct SignatureElement
{
    std::string semantic;
    uint32_t index = 0, sv = 0, type = 0, reg = 0;
    uint8_t mask = 0, used = 0;
};

// The input (ISGN) or output (OSGN) signature; empty if the chunk is not there.
std::vector<SignatureElement> Signature(const Bytes& code, const char* which = "ISGN");

struct Resource
{
    std::string name;
    std::string type;               // cbuffer, texture, sampler...
    uint32_t dimension = 0, bind = 0, count = 0;
};

struct CBufferVar
{
    std::string name;
    uint32_t offset = 0, size = 0;
    bool used = false;
};

struct CBufferInfo
{
    uint32_t size = 0;
    std::vector<CBufferVar> vars;
};

struct Reflection
{
    std::vector<Resource> resources;
    std::vector<std::pair<std::string, CBufferInfo>> cbuffers;     // in the chunk's order
    const CBufferInfo* Find(const std::string& name) const;
};

Reflection Reflect(const Bytes& code);

} // namespace hlsl
