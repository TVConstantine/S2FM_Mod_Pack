#include "render_state.h"

#include <cctype>
#include <cstring>

namespace render_state
{
namespace
{
using Table = std::map<std::string, int>;

const Table CULL = {{"NONE", 0}, {"BACK", 1}, {"FRONT", 2}, {"DEFAULT", 1}};
const Table FILL = {{"SOLID", 0}, {"WIREFRAME", 1}};
const Table COMPARISON = {{"NEVER", 0}, {"LESS", 1}, {"EQUAL", 2}, {"LESS_EQUAL", 3}, {"GREATER", 4},
                          {"NOT_EQUAL", 5}, {"GREATER_EQUAL", 6}, {"ALWAYS", 7}};
const Table BLEND = {{"ZERO", 0}, {"ONE", 1}, {"SRC_COLOR", 2}, {"INV_SRC_COLOR", 3}, {"SRC_ALPHA", 4},
                     {"INV_SRC_ALPHA", 5}, {"DEST_ALPHA", 6}, {"INV_DEST_ALPHA", 7}, {"DEST_COLOR", 8},
                     {"INV_DEST_COLOR", 9}, {"SRC_ALPHA_SAT", 10}, {"BLEND_FACTOR", 11}, {"SRC1_COLOR", 12},
                     {"INV_SRC1_COLOR", 13}, {"SRC1_ALPHA", 14}, {"INV_SRC1_ALPHA", 15}};
const Table BLEND_OP = {{"ADD", 0}, {"SUBTRACT", 1}, {"REV_SUBTRACT", 2}, {"MIN", 3}, {"MAX", 4}};
const Table BOOL = {{"TRUE", 1}, {"FALSE", 0}};
const Table MASK = {{"R", 1}, {"G", 2}, {"B", 4}, {"A", 8}, {"RGB", 7}, {"RGBA", 15}, {"NONE", 0}, {"ALL", 15}};

enum Record { RASTER = 0, DEPTH = 1, BLEND_RECORD = 2 };
enum Kind { U8, LANES, LANE0, I32, F32 };

struct StateInfo
{
    Record record;
    Kind kind;
    int offset;
    const Table* table;
};

const std::map<std::string, StateInfo> STATES = {
    {"FillMode", {RASTER, U8, 0, &FILL}},
    {"CullMode", {RASTER, U8, 1, &CULL}},
    {"DepthClipEnable", {RASTER, U8, 2, &BOOL}},
    {"DepthBias", {RASTER, I32, 4, nullptr}},
    {"DepthBiasClamp", {RASTER, F32, 8, nullptr}},
    {"SlopeScaleDepthBias", {RASTER, F32, 12, nullptr}},
    {"DepthEnable", {DEPTH, U8, 0, &BOOL}},
    {"DepthWriteEnable", {DEPTH, U8, 1, &BOOL}},
    {"DepthFunc", {DEPTH, U8, 2, &COMPARISON}},
    {"StencilEnable", {DEPTH, U8, 5, &BOOL}},
    {"StencilReadMask", {DEPTH, U8, 6, nullptr}},
    {"StencilWriteMask", {DEPTH, U8, 7, nullptr}},
    {"AlphaToCoverageEnable", {BLEND_RECORD, U8, 0, &BOOL}},
    {"BlendEnable", {BLEND_RECORD, LANES, 3, &BOOL}},
    {"SrcBlend", {BLEND_RECORD, LANES, 11, &BLEND}},
    {"DstBlend", {BLEND_RECORD, LANES, 19, &BLEND}},
    {"BlendOp", {BLEND_RECORD, LANES, 27, &BLEND_OP}},
    {"SrcBlendAlpha", {BLEND_RECORD, LANES, 35, &BLEND}},
    {"DstBlendAlpha", {BLEND_RECORD, LANES, 43, &BLEND}},
    {"BlendOpAlpha", {BLEND_RECORD, LANES, 51, &BLEND_OP}},
    {"ColorWriteEnable0", {BLEND_RECORD, LANE0, 59, &MASK}},
};

Bytes Hex(const char* hex)
{
    Bytes out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
    return out;
}

// The defaults: rasterizer and blend as vr_black_unlit has them, the depth stencil record any depth or stencil
// state starts from (depth test and write on, LESS_EQUAL, the engine's stencil values for models).
Bytes RasterDefault() { return Hex("00010101000000000000000000000000"); }
Bytes DepthTemplate() { return Hex("010103000001ff01000002070000000700000000"); }
Bytes BlendDefault()
{
    Bytes b(3, 0);
    const uint8_t lanes[8] = {0, 4, 5, 0, 4, 5, 0, 15};      // enable, src, dst, op, srcA, dstA, opA, mask
    for (uint8_t v : lanes)
        b.insert(b.end(), 8, v);
    b.insert(b.end(), 8, 0);                                 // sRGB write
    return b;
}

std::string Upper(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool IsToken(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '+' || c == '-';
}

// A condition: names joined by || or && (&& first, as in C), each one maybe with a ! before it. Terms are the ||
// parts, each a list of names that must all hold.
struct Literal
{
    std::string name;
    bool negate = false;
};
using Terms = std::vector<std::vector<Literal>>;

bool ParseCondition(const std::string& cond, Terms& out)
{
    out.clear();
    for (size_t start = 0;;)
    {
        const size_t bar = cond.find("||", start);
        const std::string term = cond.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        std::vector<Literal> all;
        for (size_t s = 0;;)
        {
            const size_t amp = term.find("&&", s);
            Literal l;
            l.name = Trim(term.substr(s, amp == std::string::npos ? std::string::npos : amp - s));
            while (!l.name.empty() && l.name[0] == '!')
            {
                l.negate = !l.negate;
                l.name = Trim(l.name.substr(1));
            }
            if (l.name.empty())
                return false;
            for (char ch : l.name)
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
                    return false;
            all.push_back(l);
            if (amp == std::string::npos)
                break;
            s = amp + 2;
        }
        out.push_back(all);
        if (bar == std::string::npos)
            break;
        start = bar + 2;
    }
    return true;
}

// What a statement's value comes to in one static combo: a word, or (runtime) a choice between a and b that the
// features with no static combo make, the static combos' part of the condition already worked out.
struct Resolved
{
    std::string word;
    bool runtime = false;
    Terms terms;
    std::string a, b;
};

// expr itself, or for "COND ? A : B" the side the condition picks; COND names static combos (known in this combo) and
// features (a feature with a static combo is known too; one without is read when the material is drawn).
Resolved Resolve(const std::string& what, const std::string& expr, const Combos& combos, const Features& features)
{
    Resolved r;
    r.word = Trim(expr);
    size_t q = expr.find('?'), c = expr.find(':');
    if (q == std::string::npos || c == std::string::npos || c < q)
        return r;
    std::string cond = Trim(expr.substr(0, q)), a = Trim(expr.substr(q + 1, c - q - 1)), b = Trim(expr.substr(c + 1));
    auto token = [](const std::string& s) {
        if (s.empty())
            return false;
        for (char ch : s)
            if (!IsToken(ch))
                return false;
        return true;
    };
    Terms terms;
    if (!token(a) || !token(b) || !ParseCondition(cond, terms))
        return r;
    bool always = false;
    for (const auto& term : terms)
    {
        bool holds = true;
        std::vector<Literal> left;
        for (const Literal& l : term)
        {
            auto it = combos.find(l.name);
            if (it != combos.end())
                holds = holds && ((it->second != 0) != l.negate);
            else if (features.count(l.name))
                left.push_back(l);
            else
                throw RenderStateError(what + ": " + l.name + " is neither a static combo of this stage nor a feature");
        }
        if (!holds)
            continue;
        if (left.empty())
            always = true;
        else
            r.terms.push_back(left);
    }
    if (always || r.terms.empty())
    {
        r.terms.clear();
        r.word = always ? a : b;
        return r;
    }
    r.runtime = true;
    r.a = a;
    r.b = b;
    return r;
}

// The engine's expression for "terms ? a : b" (VfxEval's bytecode, as Valve's vr_complex has DstBlend): 1a n reads
// feature n, 04 t f goes to t when the value is not zero and to f when it is, 07 pushes a float, 02 t goes to t, 00
// returns what is on top. Positions are bytes from the start.
Bytes Code(const Terms& terms, const Features& features, double a, double b)
{
    Bytes out;
    struct Patch
    {
        size_t at;
        int label;          // a term's start (0 up), TRUE (-1) or FALSE (-2)
    };
    std::vector<Patch> patches;
    std::vector<size_t> starts(terms.size());
    auto u16 = [&](size_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto f32 = [&](double v) {
        const float f = static_cast<float>(v);
        uint8_t bytes[4];
        std::memcpy(bytes, &f, 4);
        out.insert(out.end(), bytes, bytes + 4);
    };
    for (size_t t = 0; t < terms.size(); t++)
    {
        starts[t] = out.size();
        for (size_t j = 0; j < terms[t].size(); j++)
        {
            const Literal& l = terms[t][j];
            out.push_back(0x1A);
            out.push_back(static_cast<uint8_t>(features.at(l.name)));
            out.push_back(0x04);
            const bool last = j + 1 == terms[t].size();
            // The next name of the term starts right after this branch's two targets.
            const int next = -3, onFalse = t + 1 < terms.size() ? static_cast<int>(t + 1) : -2;
            int yes = last ? -1 : next, no = onFalse;
            if (l.negate)
                std::swap(yes, no);
            for (int label : {yes, no})
            {
                if (label == next)
                    u16(out.size() + 2 + (label == yes ? 2 : 0));
                else
                {
                    patches.push_back({out.size(), label});
                    u16(0);
                }
            }
        }
    }
    const size_t yesAt = out.size();
    out.push_back(0x07);
    f32(a);
    out.push_back(0x02);
    const size_t jumpAt = out.size();
    u16(0);
    const size_t noAt = out.size();
    out.push_back(0x07);
    f32(b);
    const size_t end = out.size();
    out.push_back(0x00);
    out[jumpAt] = static_cast<uint8_t>(end & 0xFF);
    out[jumpAt + 1] = static_cast<uint8_t>(end >> 8);
    for (const Patch& p : patches)
    {
        const size_t to = p.label == -1 ? yesAt : p.label == -2 ? noAt : starts[p.label];
        out[p.at] = static_cast<uint8_t>(to & 0xFF);
        out[p.at + 1] = static_cast<uint8_t>(to >> 8);
    }
    return out;
}

bool ParseNumber(const std::string& word, bool isFloat, double& out)
{
    try
    {
        size_t used = 0;
        out = isFloat ? std::stod(word, &used) : static_cast<double>(std::stoll(word, &used, 0));
        return used == word.size();
    }
    catch (...)
    {
        return false;
    }
}

// A state's value from its word; false for DEFAULT (left as the defaults have it).
bool StateValue(const std::string& state, const StateInfo& info, const std::string& word, double& v)
{
    if (info.table)
    {
        auto t = info.table->find(Upper(word));
        if (t != info.table->end())
        {
            v = t->second;
            return true;
        }
    }
    if (Upper(word) == "DEFAULT")
        return false;
    if (!ParseNumber(word, info.kind == F32, v))
    {
        std::string takes = "a number";
        if (info.table)
        {
            takes.clear();
            for (const auto& e : *info.table)
                takes += (takes.empty() ? "" : ", ") + e.first;
        }
        throw RenderStateError("RenderState(" + state + ", " + word + "): not a value it takes (" + takes + ")");
    }
    return true;
}

// The id Valve's files give each render state as a variable (the variable's bits), from the game's own shaders.
const std::map<std::string, int> STATE_IDS = {
    {"FillMode", 0},           {"CullMode", 1},          {"DepthBias", 2},       {"SlopeScaleDepthBias", 3},
    {"DepthBiasClamp", 4},     {"DepthEnable", 16},      {"DepthWriteEnable", 17}, {"DepthFunc", 18},
    {"StencilEnable", 21},     {"StencilReadMask", 22},  {"StencilWriteMask", 23}, {"AlphaToCoverageEnable", 37},
    {"BlendEnable", 41},       {"SrcBlend", 43},         {"DstBlend", 44},       {"BlendOp", 45},
    {"SrcBlendAlpha", 46},     {"ColorWriteEnable0", 49}};

const StateInfo& Info(const std::string& state)
{
    auto it = STATES.find(state);
    if (it == STATES.end())
    {
        std::string known;
        for (const auto& s : STATES)
            known += (known.empty() ? "" : ", ") + s.first;
        throw RenderStateError("RenderState(" + state + "): not a state this compiler knows (" + known + ")");
    }
    return it->second;
}
} // namespace

Records Build(const std::vector<std::pair<std::string, std::string>>& statements, const Combos& combos,
              const Features& features)
{
    Bytes recs[3] = {RasterDefault(), Bytes(), BlendDefault()};
    bool depth = false;
    // A record with a state that follows a feature at run time goes as variables, all of its states.
    bool asVariables[3] = {false, false, false};
    for (const auto& [state, expr] : statements)
        if (Resolve("RenderState(" + state + ")", expr, combos, features).runtime)
            asVariables[Info(state).record] = true;
    Records out;
    for (const auto& [state, expr] : statements)
    {
        const StateInfo& info = Info(state);
        const Resolved r = Resolve("RenderState(" + state + ")", expr, combos, features);
        if (asVariables[info.record])
        {
            auto id = STATE_IDS.find(state);
            if (id == STATE_IDS.end())
                throw RenderStateError("RenderState(" + state + "): this state cannot follow a feature (no variable id "
                                       "known for it)");
            StateVariable sv;
            sv.state = state;
            sv.type = info.table == &BOOL ? 9 : info.kind == F32 ? 1 : 5;
            sv.bits = id->second;
            sv.context = info.record == RASTER ? 128 : info.record == DEPTH ? 256 : 512;
            sv.elements = info.record == BLEND_RECORD ? 2 : 1;
            if (r.runtime)
            {
                double a = 0, b = 0;
                if (!StateValue(state, info, r.a, a) || !StateValue(state, info, r.b, b))
                    throw RenderStateError("RenderState(" + state + "): a state that follows a feature takes values, "
                                           "not DEFAULT");
                sv.code = Code(r.terms, features, a, b);
            }
            else if (!StateValue(state, info, r.word, sv.value))
                continue;
            out.variables.push_back(sv);
            continue;
        }
        double v = 0;
        if (!StateValue(state, info, r.word, v))
            continue;               // left as the defaults have it
        if (info.record == DEPTH && !depth)
        {
            recs[DEPTH] = DepthTemplate();
            depth = true;
        }
        Bytes& rec = recs[info.record];
        int iv = static_cast<int>(v);
        switch (info.kind)
        {
        case U8:
        case LANE0: rec[info.offset] = static_cast<uint8_t>(iv & 0xFF); break;
        case LANES:
            for (int k = 0; k < 8; k++)
                rec[info.offset + k] = static_cast<uint8_t>(iv & 0xFF);
            break;
        case I32: std::memcpy(&rec[info.offset], &iv, 4); break;
        case F32:
        {
            float f = static_cast<float>(v);
            std::memcpy(&rec[info.offset], &f, 4);
            break;
        }
        }
    }
    for (int i = 0; i < 3; i++)
    {
        bool present = (i != DEPTH || depth) && !asVariables[i];
        out.has[i] = present ? 0 : 1;
        if (present)
            out.rs[i] = recs[i];
    }
    return out;
}

std::vector<vcs::Attribute> Attributes(const std::vector<std::tuple<std::string, std::string, std::string>>& statements,
                                       const Combos& combos, const Features& features)
{
    std::vector<vcs::Attribute> out;
    for (const auto& [kind, name, expr] : statements)
    {
        const std::string what = kind + "(" + name + ")";
        const Resolved r = Resolve(what, expr, combos, features);
        vcs::Attribute a;
        a.name = name;
        a.hash = vcs::Murmur2(name);
        a.binding = -1;
        a.type = kind == "BoolAttribute" ? 9 : kind == "IntAttribute" ? 5 : 1;
        auto value = [&](const std::string& word, double& v) {
            if (kind == "BoolAttribute")
            {
                const std::string w = Upper(word);
                if (w == "TRUE" || w == "FALSE")
                {
                    v = w == "TRUE" ? 1 : 0;
                    return true;
                }
            }
            return ParseNumber(word, kind == "FloatAttribute", v);
        };
        if (r.runtime)
        {
            double va = 0, vb = 0;
            if (!value(r.a, va) || !value(r.b, vb))
                throw RenderStateError(what + ": " + r.a + " or " + r.b + " is not a value of that kind");
            a.expression = Code(r.terms, features, va, vb);
            out.push_back(a);
            continue;
        }
        double v = 0;
        if (!value(r.word, v))
            throw RenderStateError(what + ", " + r.word + ": not a value of that kind");
        a.b = static_cast<uint8_t>(static_cast<int>(v));
        a.i = static_cast<int32_t>(v);
        a.f = static_cast<float>(v);
        out.push_back(a);
    }
    return out;
}

} // namespace render_state
