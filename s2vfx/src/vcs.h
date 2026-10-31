// Half-Life: Alyx's compiled shader files (VCS version 64, "vcs2"): read and written back byte for byte.
// The layout is described in docs/format_v64.md; python/vcs64.py is the reference this follows.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bytes.h"

namespace vcs
{

// A fixed length string field from text, zero padded; and the text of one.
Bytes Fixed(const std::string& s, size_t n);
std::string Text(const Bytes& field);

struct Combo
{
    Bytes name, alias;                  // 64 bytes each
    int32_t type = 0, min = 0, max = 0, source = 0, feature = -1;
    std::vector<std::string> states;
    int32_t compare = 0;                // only for sources 10 and 11
};

struct Rule
{
    int32_t method = 0, type = 0;
    std::array<uint8_t, 16> argTypes{};
    std::array<int32_t, 16> argIndices{}, argValues{}, extra{};
    Bytes error;                        // 256 bytes
};

struct Variable
{
    Bytes name, group;                  // 64 each
    int32_t ui = 0;
    float step = 0.0f;
    Bytes sourceString;                 // 64
    int32_t source = 0;
    Bytes expression;                   // sources 6 and 7
    std::optional<Bytes> visibility;    // a UI visibility expression ("SBMS")
    int32_t sourceIndex = -1;
    int32_t type = 0, reg = 0, context = 0, elements = 0, bits = -1;
    Bytes defaultTexture;               // 64
    std::array<int32_t, 4> intDefs{}, intMins{}, intMaxs{};
    std::array<float, 4> floatDefs{}, floatMins{}, floatMaxs{};
    int32_t format = -1, channels = 0;
    std::array<int32_t, 4> channelInfo{-1, -1, -1, -1};
    int32_t colorSpace = 0, precision = -1;
    Bytes fileEnding, processing;       // 32 each
};

struct Channel
{
    uint32_t channel = 0;
    std::array<int32_t, 4> inputs{-1, -1, -1, -1};
    int32_t colorSpace = 0;
    Bytes command;                      // 256
};

struct CBufferVar
{
    Bytes name;                         // 64
    int32_t offset = 0, vector = 0, rows = 0, elements = 0;
};

struct CBuffer
{
    Bytes name;                         // 64
    uint32_t size = 0;
    int32_t type = 0;
    std::vector<CBufferVar> vars;
    uint32_t crc = 0;
};

struct SignatureEntry
{
    std::string name, d3dSemantic, semantic;
    int32_t index = 0;
    bool operator==(const SignatureEntry& o) const
    {
        return name == o.name && d3dSemantic == o.d3dSemantic && semantic == o.semantic && index == o.index;
    }
};
using Signature = std::vector<SignatureEntry>;

struct Mode
{
    Bytes name, fallback;               // 64 each
    std::vector<std::pair<Bytes, int32_t>> combos;
};

struct Block
{
    int64_t id = 0;
    Bytes data;                         // the static combo, decoded
    Bytes stored;                       // as it is in the file
};

struct File
{
    std::string program;                // features, vs, ps...
    int32_t version = 64;
    uint32_t flags = 0;
    int32_t headerVersion = 1;          // features only
    std::string description;
    int32_t dev = 0;
    std::vector<int32_t> available;
    std::vector<Mode> modes;
    std::vector<Bytes> programHashes;   // 16 bytes each
    Bytes variableHash;                 // 16
    int32_t variableSourceMax = 0;
    std::vector<Combo> statics, dynamics;
    std::vector<Rule> staticRules, dynamicRules;
    std::vector<Variable> variables;
    std::vector<Channel> channels;
    std::vector<CBuffer> cbuffers;
    std::vector<Signature> signatures;  // features and vs
    std::vector<Block> blocks;
};

// ---------------------------------------------------------------------------------------------- static combos
struct Field
{
    uint16_t var = 0, slot = 0;
    bool operator==(const Field& o) const { return var == o.var && slot == o.slot; }
};

struct IndexArray
{
    int32_t firstRenderState = 0, firstConstant = 0;
    std::vector<Field> fields;
};

struct Attribute
{
    std::string name;
    uint32_t hash = 0;
    uint8_t type = 0;
    int16_t binding = -1;
    std::optional<Bytes> expression;
    float f = 0.0f;                     // type 1
    int32_t i = 0;                      // type 5
    uint8_t b = 0;                      // type 9
    std::string s;                      // type 41
    std::vector<float> v;               // types 2 to 4
};

struct CodeFile
{
    std::shared_ptr<const Bytes> data;  // the code, shared by every static combo holding it (null: none)
    Bytes hash;                         // MD5 of the code
    const Bytes& code() const
    {
        static const Bytes none;
        return data ? *data : none;
    }
};

struct State
{
    int64_t dyn = 0;
    int32_t file = 0, fileOffset = 0;
    std::array<uint8_t, 3> has{1, 1, 1};            // per rasterizer, depth stencil, blend: 0 = present
    std::array<Bytes, 3> rs;
    uint8_t hsArg = 0;
};

struct StaticCombo
{
    IndexArray all;
    std::vector<Attribute> attributes;
    std::vector<int16_t> vsInputs;
    std::vector<IndexArray> dynamic;
    std::vector<std::pair<uint8_t, uint8_t>> cbSlots;
    int32_t cbSize = 0;
    uint8_t staticCB = 1;
    uint8_t glsl = 0;
    std::vector<CodeFile> files;
    std::vector<State> states;
};

constexpr size_t RS_SIZES[3] = {16, 20, 75};

uint32_t Murmur2(const std::string& name, uint32_t seed = 0x31415926);
Bytes Md5(const Bytes& data);

StaticCombo ReadStaticCombo(const Bytes& blob, const std::string& program);
Bytes WriteStaticCombo(const StaticCombo& sc, const std::string& program);

// ---------------------------------------------------------------------------------------------- files
enum class Pack { Keep, Raw, Stored, Lzma };

File Read(const Bytes& data, const std::string& program);
Bytes Write(const File& f, Pack pack = Pack::Keep);

Bytes PackBlock(const Bytes& blob, Pack how);
// Pack::Lzma's compression level, 0..9 (9 as the game's files have it; lower packs much faster, a little larger).
void SetLzmaLevel(int level);

} // namespace vcs
