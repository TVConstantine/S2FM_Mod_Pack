#include "compiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "hlsl.h"
#include "render_state.h"
#include "vfx.h"
#include "vpk.h"

namespace
{
using vcs::Field;
using vcs::Variable;

const std::map<std::string, int> VAR_TYPES = {{"float", 1}, {"float2", 2}, {"float3", 3}, {"float4", 4},
                                               {"int", 5},   {"int2", 6},   {"int3", 7},   {"int4", 8},
                                               {"bool", 9},  {"Texture2D", 14}, {"Texture3D", 15}, {"TextureCube", 16}};
enum Reg { REG_BOOL = 1, REG_INT4 = 2, REG_FLOAT4 = 3, REG_TEXTURE = 4, REG_RENDER_STATE = 5, REG_SAMPLER = 6,
           REG_INPUT_TEXTURE = 7, REG_CBUFFER = 8 };
const std::map<std::string, int> UI_TYPES = {{"none", 0},       {"slider", 1},   {"color", 2}, {"texture", 3},
                                              {"vectortext", 4}, {"checkbox", 5}, {"enum", 6}};
// Sampler states: the variable's bits field says which state; the values are D3D11's filters and Valve's address
// modes (0 wrap, 1 mirror, 2 clamp, 3 border, 4 mirror once).
const std::map<std::string, int> SAMPLER_STATES = {{"Filter", 0},   {"AddressU", 1},    {"AddressV", 2},
                                                    {"AddressW", 3}, {"BorderColor", 4}, {"MaxAniso", 7}};
const std::map<std::string, int> FILTERS = {
    {"MIN_MAG_MIP_POINT", 0},    {"POINT", 0},       {"MIN_MAG_MIP_LINEAR", 0x15},
    {"LINEAR", 0x15},            {"BILINEAR", 0x14}, {"MIN_MAG_LINEAR_MIP_POINT", 0x14},
    {"ANISOTROPIC", 0x55},       {"ANISO", 0x55},    {"COMPARISON_MIN_MAG_MIP_LINEAR", 0x95},
    {"COMPARISON_LINEAR", 0x95}};    // comparison: shadow maps
const std::map<std::string, int> ADDRESS = {{"WRAP", 0}, {"MIRROR", 1}, {"CLAMP", 2}, {"BORDER", 3}, {"MIRROR_ONCE", 4}};
const std::map<std::string, int> FORMATS = {{"RGBA8888", 0}, {"DXT1", 13},  {"DXT3", 14}, {"DXT5", 15},
                                             {"ATI2N", 36},   {"ATI1N", 37}, {"BC6H", 85}, {"BC7", 86}};
const std::map<std::string, std::vector<int>> CHANNELS = {{"R", {0}},       {"G", {1}},          {"B", {2}},
                                                           {"A", {3}},       {"RG", {0, 1}},      {"RGB", {0, 1, 2}},
                                                           {"RGBA", {0, 1, 2, 3}}};
const std::map<std::string, std::string> TARGETS = {{"vs", "vs_5_0"}, {"ps", "ps_5_0"}};
const std::map<std::string, std::string> ENTRIES = {{"vs", "MainVs"}, {"ps", "MainPs"}};

std::string Lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Strip(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n\f\v"), b = s.find_last_not_of(" \t\r\n\f\v");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Python's round(): halves to even.
int RoundEven(double x) { return static_cast<int>(std::nearbyint(x)); }

const std::vector<std::string>* Ann(const std::vector<vfx::Call>& annotations, const std::string& name)
{
    for (const auto& c : annotations)
        if (c.name == name)
            return &c.args;
    return nullptr;
}

Variable Blank(const std::string& name)
{
    Variable v;
    v.name = vcs::Fixed(name, 64);
    v.group = vcs::Fixed("", 64);
    v.sourceString = vcs::Fixed("", 64);
    v.defaultTexture = vcs::Fixed("", 64);
    v.fileEnding = vcs::Fixed("", 32);
    v.processing = vcs::Fixed("", 32);
    // No range: Valve's files say unbounded. Zero here is a range of nothing: an input texture's values are clamped
    // to it when the material compiles (a normal map comes out flat).
    v.intMins.fill(-999999999);
    v.intMaxs.fill(999999999);
    v.floatMins.fill(-1e9f);
    v.floatMaxs.fill(1e9f);
    return v;
}

// A material parameter: set by the artist, kept in the material's constant buffer.
Variable ParamVariable(const vfx::Annotated& p)
{
    const std::string& t = p.type;
    if (!VAR_TYPES.count(t))
        throw CompileError("parameter " + p.name + ": type " + t + " is not supported");
    int n = std::isdigit(static_cast<unsigned char>(t.back())) ? t.back() - '0' : 1;
    Variable v = Blank(p.name);
    const auto& a = p.annotations;
    auto ui = Ann(a, "UiType");
    v.ui = 0;
    if (ui && !ui->empty() && UI_TYPES.count(Lower((*ui)[0])))
        v.ui = UI_TYPES.at(Lower((*ui)[0]));
    auto group = Ann(a, "UiGroup");
    v.group = vcs::Fixed(vfx::Unquote(group && !group->empty() ? (*group)[0] : "\"\""), 64);
    v.type = VAR_TYPES.at(t);
    v.reg = t == "bool" ? REG_BOOL : t.rfind("int", 0) == 0 ? REG_INT4 : REG_FLOAT4;
    v.context = 8192;
    v.elements = n;
    const std::vector<std::string>* d = Ann(a, "Default" + std::to_string(n));
    if (!d || d->empty())
        d = Ann(a, "Default");
    std::vector<double> defaults;
    if (d && !d->empty())
        for (const auto& x : *d)
            defaults.push_back(std::stod(x));
    else
        defaults.assign(n, 0.0);
    defaults.resize(std::max<size_t>(defaults.size(), 4), 0.0);
    const std::vector<std::string>* rg = Ann(a, "Range" + std::to_string(n));
    if (!rg || rg->empty())
        rg = Ann(a, "Range");
    std::vector<double> rng;
    if (rg)
        for (const auto& x : *rg)
            rng.push_back(std::stod(x));
    std::vector<double> lo, hi;
    if (!rng.empty())
    {
        lo.assign(rng.begin(), rng.begin() + rng.size() / 2);
        hi.assign(rng.begin() + rng.size() / 2, rng.end());
    }
    lo.resize(lo.size() + 4, 0.0);
    hi.resize(hi.size() + 4, 1.0);
    for (int k = 0; k < 4; k++)
    {
        v.floatDefs[k] = static_cast<float>(defaults[k]);
        v.intDefs[k] = RoundEven(defaults[k]);
        if (rng.empty())
            continue;               // unbounded, as Blank has it
        v.floatMins[k] = static_cast<float>(lo[k]);
        v.floatMaxs[k] = static_cast<float>(hi[k]);
        v.intMins[k] = static_cast<int>(lo[k]);
        v.intMaxs[k] = static_cast<int>(hi[k]);
    }
    return v;
}

Variable InputTextureVariable(const vfx::InputTexture& t)
{
    Variable v = Blank(t.name);
    v.group = vcs::Fixed(t.group, 64);
    v.ui = UI_TYPES.at("texture");
    v.type = VAR_TYPES.at("Texture2D");
    v.reg = REG_INPUT_TEXTURE;
    v.context = 2048;
    v.elements = -1;
    v.colorSpace = Lower(t.colorSpace) == "srgb" ? 1 : 0;
    v.precision = t.bits;
    std::string suffix = t.suffix;
    suffix.erase(0, suffix.find_first_not_of('_') == std::string::npos ? suffix.size() : suffix.find_first_not_of('_'));
    v.fileEnding = vcs::Fixed(suffix, 32);
    v.processing = vcs::Fixed(t.processing, 32);
    for (int k = 0; k < 4; k++)
    {
        v.floatDefs[k] = t.defaults[k];
        v.intDefs[k] = RoundEven(t.defaults[k]);
    }
    return v;
}

Variable TextureVariable(const vfx::Annotated& t, const std::vector<int>& channelIndices)
{
    const auto& a = t.annotations;
    Variable v = Blank(t.name);
    v.ui = UI_TYPES.at("texture");
    if (!VAR_TYPES.count(t.type))
        throw CompileError("texture " + t.name + ": type " + t.type + " is not supported");
    v.type = VAR_TYPES.at(t.type);
    v.reg = REG_TEXTURE;
    v.context = 6144;
    v.elements = 1;
    auto srgb = Ann(a, "SrgbRead");
    v.bits = srgb && !srgb->empty() && Lower((*srgb)[0]) == "true" ? 1 : 0;
    auto fmtArgs = Ann(a, "OutputFormat");
    std::string fmt = fmtArgs && !fmtArgs->empty() ? (*fmtArgs)[0] : "DXT5";
    if (!FORMATS.count(fmt))
        throw CompileError("texture " + t.name + ": output format " + fmt + " is not known");
    v.format = FORMATS.at(fmt);
    v.channels = static_cast<int>(channelIndices.size());
    for (int k = 0; k < 4; k++)
        v.channelInfo[k] = k < static_cast<int>(channelIndices.size()) ? channelIndices[k] : -1;
    return v;
}

struct SamplerValue
{
    int i = 0;
    std::array<float, 4> f{};
    std::string key;
};

SamplerValue SamplerStateValue(const std::string& state, const std::vector<std::string>& args)
{
    SamplerValue v;
    if (state == "BorderColor")
    {
        for (int k = 0; k < 4; k++)
            v.f[k] = k < static_cast<int>(args.size()) ? std::stof(args[k]) : 0.0f;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g", v.f[0], v.f[1], v.f[2], v.f[3]);
        v.key = buf;
        return v;
    }
    if (args.empty())
        throw CompileError("sampler state " + state + "() has no value");
    if (state == "MaxAniso")
        v.i = std::stoi(args[0]);
    else
    {
        const auto& table = state == "Filter" ? FILTERS : ADDRESS;
        if (!table.count(args[0]))
            throw CompileError("sampler state " + state + "(" + args[0] + ") is not known");
        v.i = table.at(args[0]);
    }
    v.key = std::to_string(v.i);
    return v;
}

Variable SamplerStateVariable(const std::string& state, const SamplerValue& value)
{
    Variable v = Blank(state);
    v.type = 5;
    v.reg = REG_SAMPLER;
    v.source = 5;               // a render state literal
    v.context = 4096;
    v.bits = SAMPLER_STATES.at(state);
    if (state == "BorderColor")
    {
        v.type = 4;
        v.floatDefs = value.f;
    }
    else
        v.intDefs = {value.i, 0, 0, 0};
    return v;
}

// A render state as a variable (as Valve's translucent variants have their blend): a literal, or an expression of the
// material's features the engine works out (render_state.h).
Variable RenderStateVariable(const render_state::StateVariable& s)
{
    Variable v = Blank(s.state);
    v.type = s.type;
    v.reg = REG_RENDER_STATE;
    v.context = s.context;
    v.elements = s.elements;
    v.bits = s.bits;
    if (!s.code.empty())
    {
        v.source = 6;
        v.expression = s.code;
    }
    else
    {
        v.source = 5;
        v.intDefs = {static_cast<int32_t>(s.value), 0, 0, 0};
        v.floatDefs = {static_cast<float>(s.value), 0, 0, 0};
    }
    return v;
}

uint32_t ChannelMask(const std::string& spec)
{
    if (!CHANNELS.count(spec))
        throw CompileError("channel " + spec + " is not known");
    const auto& idx = CHANNELS.at(spec);
    uint32_t m = 0;
    for (size_t i = 0; i < idx.size(); i++)
        m |= static_cast<uint32_t>(idx[i]) << (8 * i);
    for (size_t i = idx.size(); i < 4; i++)
        m |= 0xFFu << (8 * i);
    return m;
}

struct ComboDef
{
    std::string name;
    int min = 0, max = 0, feature = -1;
    std::string uiFlag;     // UiOnly(param): not a define of the code (vfx.h)
};

// A hidden value the engine works out itself (source 6, an expression), as Valve's g_fInvLightRangeForSelfShadowNormals
// is kept: no UI group, so the Material Editor does not show it. The expression is a float constant: op 07 (push the
// float that follows), op 00 (return).
Variable ConstantVariable(const std::string& name, float value)
{
    Variable v = Blank(name);
    v.ui = UI_TYPES.at("slider");
    v.source = 6;
    v.expression = {0x07};
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    v.expression.insert(v.expression.end(), p, p + 4);
    v.expression.push_back(0x00);
    v.type = VAR_TYPES.at("float");
    v.reg = REG_FLOAT4;
    v.context = 8192;
    v.elements = 1;
    return v;
}

using Values = std::vector<std::pair<std::string, int>>;    // combo name: value, in the combos' order

// Every combination, in id order (the first combo varies fastest).
std::vector<Values> AllValues(const std::vector<ComboDef>& combos)
{
    std::vector<Values> out;
    std::vector<int> cur;
    for (const auto& c : combos)
        cur.push_back(c.min);
    for (;;)
    {
        Values v;
        for (size_t i = 0; i < combos.size(); i++)
            v.emplace_back(combos[i].name, cur[i]);
        out.push_back(v);
        size_t i = 0;
        for (; i < combos.size(); i++)
        {
            if (++cur[i] <= combos[i].max)
                break;
            cur[i] = combos[i].min;
        }
        if (i == combos.size())
            break;
    }
    return out;
}

// One stage's file: its variables, channels, combos and compiled blocks.
struct Program
{
    std::string stage;
    const vfx::Stage* st = nullptr;
    const std::vector<vfx::Feature>* features = nullptr;
    std::vector<Variable> variables;
    std::map<std::string, int> index;
    std::vector<vcs::Channel> channels;
    std::map<std::string, int> textureVars, paramVars;
    std::map<std::string, std::vector<int>> samplerVars;
    std::map<int, std::string> uiFeature;   // variable: the feature that shows it (UiFeature(F_...))

    int Add(const std::string& key, const Variable& v)
    {
        auto it = index.find(key);
        if (it != index.end())
            return it->second;
        int i = static_cast<int>(variables.size());
        index[key] = i;
        variables.push_back(v);
        return i;
    }

    // The stage's own variables; engine variables are added when a variant binds them.
    void Declare()
    {
        std::map<std::string, int> inputs;
        for (const auto& t : st->inputs)
            inputs[t.name] = Add("input:" + t.name, InputTextureVariable(t));
        for (const auto& t : st->textures)
        {
            std::vector<int> idx;
            for (const auto& c : t.annotations)
            {
                if (c.name != "Channel")
                    continue;
                if (c.args.size() < 2)
                    throw CompileError("texture " + t.name + ": Channel(spec, Box(input), space)");
                std::string spec = c.args[0], src = c.args[1], space = c.args.size() > 2 ? c.args[2] : "Linear";
                size_t paren = src.find('(');
                if (paren == std::string::npos)
                    throw CompileError("texture " + t.name + ": " + src + " is not a channel processor");
                // A processor takes up to four input textures: HemiOctIsoRoughness_RG_B(TextureNormal, TextureRoughness).
                std::string command = Strip(src.substr(0, paren));
                std::string inner = src.substr(paren + 1);
                while (!inner.empty() && inner.back() == ')')
                    inner.pop_back();
                std::vector<std::string> sources;
                for (size_t start = 0;;)
                {
                    size_t comma = inner.find(',', start);
                    std::string s = Strip(inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                    if (!s.empty())
                        sources.push_back(s);
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
                for (const auto& source : sources)
                    if (!inputs.count(source))
                        throw CompileError("texture " + t.name + ": input texture " + source + " is not declared");
                if (sources.empty() || sources.size() > 4)
                    throw CompileError("texture " + t.name + ": " + command + " takes one to four input textures");
                vcs::Channel ch;
                ch.channel = ChannelMask(spec);
                for (size_t k = 0; k < 4; k++)
                    ch.inputs[k] = k < sources.size() ? inputs[sources[k]] : -1;
                ch.colorSpace = Lower(space) == "srgb" ? 1 : 0;
                ch.command = vcs::Fixed(command, 256);
                channels.push_back(ch);
                idx.push_back(static_cast<int>(channels.size()) - 1);
            }
            textureVars[t.name] = Add("texture:" + t.name, TextureVariable(t, idx));
            if (auto f = Ann(t.annotations, "UiFeature"); f && !f->empty())
                uiFeature[textureVars[t.name]] = (*f)[0];
        }
        for (const auto& s : st->samplers)
        {
            auto& list = samplerVars[s.name];
            list.clear();
            for (const auto& c : s.annotations)
            {
                if (!SAMPLER_STATES.count(c.name))
                    throw CompileError("sampler " + s.name + ": state " + c.name + " is not supported");
                SamplerValue value = SamplerStateValue(c.name, c.args);
                list.push_back(Add("state:" + c.name + ":" + value.key, SamplerStateVariable(c.name, value)));
            }
        }
        for (const auto& p : st->params)
        {
            Variable v = ParamVariable(p);
            // FeatureToInt(F_X) (or FeatureToBool, FeatureToFloat): the engine sets it to the material's feature value,
            // so code can follow a feature with no static combo (Valve's blend shader has its g_nDebugMode so).
            static const std::pair<const char*, int> kFrom[] = {{"FeatureToInt", 2}, {"FeatureToBool", 3},
                                                                 {"FeatureToFloat", 4}};
            for (const auto& [kind, source] : kFrom)
                if (auto f = Ann(p.annotations, kind); f && !f->empty())
                {
                    int fi = -1;
                    for (size_t i = 0; i < features->size(); i++)
                        if ((*features)[i].name == (*f)[0])
                            fi = static_cast<int>(i);
                    if (fi < 0)
                        throw CompileError("parameter " + p.name + ": " + kind + "(" + (*f)[0] + "): no such feature");
                    v.source = source;
                    v.sourceIndex = fi;
                    v.ui = 0;
                    v.group = vcs::Fixed("", 64);
                }
            paramVars[p.name] = Add("param:" + p.name, v);
            if (auto f = Ann(p.annotations, "UiFeature"); f && !f->empty())
                uiFeature[paramVars[p.name]] = (*f)[0];
        }
    }

    void Combos(std::vector<ComboDef>& statics, std::vector<ComboDef>& dynamics) const
    {
        for (const auto& c : st->statics)
        {
            if (!c.feature.empty())
            {
                int fi = -1;
                for (size_t i = 0; i < features->size(); i++)
                    if ((*features)[i].name == c.feature)
                        fi = static_cast<int>(i);
                if (fi < 0)
                    throw CompileError("static combo " + c.name + ": feature " + c.feature + " is not declared");
                statics.push_back({c.name, (*features)[fi].min, (*features)[fi].max, fi, c.uiFlag});
            }
            else
                statics.push_back({c.name, c.min, c.max, -1, ""});
        }
        for (const auto& c : st->dynamics)
            dynamics.push_back({c.name, c.min, c.max, -1, ""});
    }
};

vcs::Combo ComboRecord(const ComboDef& c, int kind)
{
    vcs::Combo r;
    r.name = vcs::Fixed(c.name, 64);
    r.alias = vcs::Fixed("", 64);
    r.type = kind;
    r.min = c.min;
    r.max = c.max;
    r.source = 0;
    r.feature = c.feature;
    return r;
}

struct Bound
{
    std::vector<Field> state, constants;
    bool hasMaterial = false;
    uint32_t materialSlot = 0, materialSize = 0;
};

Field MakeField(int var, uint32_t slot) { return {static_cast<uint16_t>(var), static_cast<uint16_t>(slot)}; }

// The index array of one compiled variant: bindings (textures, samplers and the engine's buffers by slot), material
// values by dword offset in the material's buffer, and the material buffer's slot and size.
Bound BindFields(Program& prog, const hlsl::Reflection& refl, const Library& lib)
{
    Bound b;
    for (const auto& r : refl.resources)
    {
        const std::string& name = r.name;
        if (r.type == "texture")
        {
            int var;
            if (prog.textureVars.count(name))
                var = prog.textureVars[name];
            else if (lib.variables.count(name))
                var = prog.Add("engine:" + name, lib.variables.at(name));
            else
                throw CompileError(prog.stage + ": texture " + name + " is neither declared nor known to the engine");
            b.state.push_back(MakeField(var, r.bind));
        }
        else if (r.type == "sampler")
        {
            if (!prog.samplerVars.count(name))
                throw CompileError(prog.stage + ": sampler " + name + " has no states (declare it with < Filter(...); >)");
            for (int var : prog.samplerVars[name])
                b.state.push_back(MakeField(var, r.bind));
        }
        else if (r.type == "cbuffer")
        {
            if (name == "$Globals")
            {
                const hlsl::CBufferInfo* cb = refl.Find(name);
                b.hasMaterial = true;
                b.materialSlot = r.bind;
                b.materialSize = cb ? cb->size : 0;
                if (!cb)
                    continue;
                for (const auto& v : cb->vars)
                {
                    if (!v.used)
                        continue;
                    int var;
                    auto lv = lib.variables.find(v.name);
                    if (prog.paramVars.count(v.name))
                        var = prog.paramVars[v.name];
                    else if (lv != lib.variables.end() && lv->second.source == 1)
                        // A value the engine sets (g_bIsSFM, g_bCubemapNormalization...), kept in the material's
                        // buffer like a parameter.
                        var = prog.Add("engine:" + v.name, lv->second);
                    else
                        throw CompileError(prog.stage + ": global " + v.name + " is not a declared parameter");
                    b.constants.push_back(MakeField(var, v.offset / 4));
                }
            }
            else if (lib.variables.count(name))
            {
                // The engine's buffers are bindings, listed with the textures and samplers; the part after
                // firstConstant holds only material values (variable, dword offset in the material's buffer).
                int var = prog.Add("engine:" + name, lib.variables.at(name));
                auto c = lib.control.find({prog.stage, name});
                int control = c == lib.control.end() ? 0 : c->second;
                b.state.push_back(MakeField(var, r.bind | (control << 8)));
            }
            else
                throw CompileError(prog.stage + ": constant buffer " + name + " is not known to the engine");
        }
        else
            throw CompileError(prog.stage + ": " + name + " (" + r.type + ") is not supported yet");
    }
    return b;
}

bool WritesNothing(const Bytes& code)
{
    return hlsl::Signature(code, "OSGN").empty() && hlsl::Disassemble(code).find("discard") == std::string::npos;
}

std::string Describe(const Values& v)
{
    std::string s = "{";
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? ", " : "") + v[i].first + ": " + std::to_string(v[i].second);
    return s + "}";
}

struct Built
{
    std::vector<vcs::StaticCombo> combos;
    std::vector<int64_t> ids;
    std::vector<vcs::Signature> signatures;
    std::vector<ComboDef> statics, dynamics;
};

// Puts the program's variables in byte order of their names, as every vs and ps file of the game has them, and
// renumbers what points at them. The engine looks its own variables up in this order.
void SortVariables(Program& prog, std::vector<vcs::StaticCombo>& combos)
{
    std::vector<int> order(prog.variables.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return vcs::Text(prog.variables[a].name) < vcs::Text(prog.variables[b].name);
    });
    std::vector<int> renum(order.size());
    std::vector<Variable> sorted;
    for (size_t i = 0; i < order.size(); i++)
    {
        renum[order[i]] = static_cast<int>(i);
        sorted.push_back(prog.variables[order[i]]);
    }
    prog.variables = std::move(sorted);
    for (auto& [k, v] : prog.index)
        v = renum[v];
    for (auto& [k, v] : prog.textureVars)
        v = renum[v];
    for (auto& [k, v] : prog.paramVars)
        v = renum[v];
    for (auto& [k, vs] : prog.samplerVars)
        for (auto& v : vs)
            v = renum[v];
    for (auto& c : prog.channels)
        for (auto& i : c.inputs)
            if (i >= 0)
                i = renum[i];
    for (auto& sc : combos)
    {
        for (auto& f : sc.all.fields)
            f.var = static_cast<uint16_t>(renum[f.var]);
        for (auto& a : sc.dynamic)
            for (auto& f : a.fields)
                f.var = static_cast<uint16_t>(renum[f.var]);
    }
}

// Every variant of a program through the HLSL compiler, on all cores: d3dcompiler compiles one shader per call, and
// a lit shader has hundreds of variants (lit.vfx: 720, 340 s on one core). The results come back in the jobs' order,
// so the files written are the same as a one by one compile's.
std::vector<Bytes> CompileAll(const std::string& stage, const std::string& source, const std::string& fileName,
                              const std::vector<hlsl::Defines>& jobs, int threadCount)
{
    std::vector<Bytes> codes(jobs.size());
    std::vector<std::string> errors(jobs.size());
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    // S2VFXC_TIMES set: each variant's compile time on stderr (what makes a shader slow to build).
    const bool timed = std::getenv("S2VFXC_TIMES") != nullptr;
    std::mutex printing;
    // S2VFXC_PROBE=n: only the n-th ps variant is compiled, timed, and the tool stops (for trying a change quickly).
    if (const char* probe = std::getenv("S2VFXC_PROBE"); probe && stage == "ps")
    {
        const size_t i = std::strtoul(probe, nullptr, 10);
        const auto start = std::chrono::steady_clock::now();
        const Bytes code = hlsl::Compile(source, ENTRIES.at(stage), TARGETS.at(stage), jobs.at(i), fileName);
        std::fprintf(stderr, "probe %zu: %.1f s, %zu bytes\n", i,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), code.size());
        std::exit(0);
    }
    auto work = [&]() {
        for (size_t i; !failed && (i = next++) < jobs.size();)
        {
            try
            {
                const auto start = std::chrono::steady_clock::now();
                codes[i] = hlsl::Compile(source, ENTRIES.at(stage), TARGETS.at(stage), jobs[i], fileName);
                if (timed)
                {
                    std::string d;
                    for (const auto& [k, v] : jobs[i])
                        d += (d.empty() ? "" : " ") + k + "=" + v;
                    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    std::lock_guard<std::mutex> lock(printing);
                    std::fprintf(stderr, "time %s %.1f s %zu bytes {%s}\n", stage.c_str(), s, codes[i].size(), d.c_str());
                }
            }
            catch (const hlsl::CompileError& e)
            {
                errors[i] = e.what();
                failed = true;
            }
        }
    };
    const size_t want = threadCount > 0 ? static_cast<size_t>(threadCount) : std::thread::hardware_concurrency();
    const size_t n = std::max<size_t>(1, std::min<size_t>(want, jobs.size()));
    std::vector<std::thread> threads;
    for (size_t t = 1; t < n; t++)
        threads.emplace_back(work);
    work();
    for (auto& t : threads)
        t.join();
    for (size_t i = 0; i < jobs.size(); i++)
        if (!errors[i].empty())
        {
            std::string d;
            for (const auto& [k, v] : jobs[i])
                d += (d.empty() ? "" : ", ") + k + "=" + v;
            throw CompileError(stage + " {" + d + "}: " + errors[i]);
        }
    return codes;
}

Built CompileProgram(const std::string& shaderName, Program& prog, const Library& lib, const CompileOptions& options,
                     const std::filesystem::path& includeDir)
{
    Built out;
    prog.Combos(out.statics, out.dynamics);
    const std::string& source = prog.st->hlsl;
    std::string fileName = (includeDir / (shaderName + "_" + prog.stage + ".hlsl")).string();
    const auto staticValues = AllValues(out.statics);
    const auto dynValues = AllValues(out.dynamics);
    // The code is compiled for the static combos that are defines (not UiOnly) only; every static combo takes the
    // code of its values of those. Code combo c, dynamic d is job c * dynamics + d.
    std::vector<ComboDef> codeStatics;
    for (const auto& c : out.statics)
        if (c.uiFlag.empty())
            codeStatics.push_back(c);
    const auto codeValues = AllValues(codeStatics);
    auto codeIndex = [&](const Values& sv) {
        size_t id = 0, mul = 1, k = 0;
        for (size_t i = 0; i < out.statics.size(); i++)
        {
            if (!out.statics[i].uiFlag.empty())
                continue;
            id += static_cast<size_t>(sv[i].second - codeStatics[k].min) * mul;
            mul *= static_cast<size_t>(codeStatics[k].max - codeStatics[k].min + 1);
            k++;
        }
        return id;
    };
    // The defines of each variant: the extra ones, then the static and dynamic combos' values (a later one replaces
    // an earlier one of the same name in its place).
    std::vector<hlsl::Defines> jobs;
    for (const auto& sv : codeValues)
        for (const auto& dv : dynValues)
        {
            hlsl::Defines defines;
            auto set = [&](const std::string& k, const std::string& v) {
                for (auto& d : defines)
                    if (d.first == k)
                    {
                        d.second = v;
                        return;
                    }
                defines.emplace_back(k, v);
            };
            for (const auto& [k, v] : options.defines)
                set(k, v);
            for (const auto& [k, v] : sv)
                set(k, std::to_string(v));
            for (const auto& [k, v] : dv)
                set(k, std::to_string(v));
            jobs.push_back(std::move(defines));
        }
    // --only: a variant whose combo has another value takes the code of the variant with that value instead (the
    // same values otherwise), so only those are compiled.
    std::vector<size_t> codeOf(jobs.size());
    {
        auto key = [](const Values& sv, const Values& dv) {
            std::string k;
            for (const auto& [n, v] : sv)
                k += n + "=" + std::to_string(v) + " ";
            for (const auto& [n, v] : dv)
                k += n + "=" + std::to_string(v) + " ";
            return k;
        };
        auto pinned = [&](Values values, const std::vector<ComboDef>& defs) {
            for (auto& [n, v] : values)
                for (const auto& [on, ov] : options.only)
                    if (n == on)
                    {
                        for (const auto& d : defs)
                            if (d.name == n && (ov < d.min || ov > d.max))
                                throw CompileError("--only " + n + "=" + std::to_string(ov) + ": out of range");
                        v = ov;
                    }
            return values;
        };
        std::map<std::string, size_t> jobOf;
        for (size_t c = 0; c < codeValues.size(); c++)
            for (size_t d = 0; d < dynValues.size(); d++)
                jobOf[key(codeValues[c], dynValues[d])] = c * dynValues.size() + d;
        for (size_t c = 0; c < codeValues.size(); c++)
            for (size_t d = 0; d < dynValues.size(); d++)
                codeOf[c * dynValues.size() + d] =
                    jobOf.at(key(pinned(codeValues[c], codeStatics), pinned(dynValues[d], out.dynamics)));
    }
    std::vector<hlsl::Defines> compiled;
    std::vector<size_t> slot(jobs.size());
    for (size_t j = 0; j < jobs.size(); j++)
        if (codeOf[j] == j)
        {
            slot[j] = compiled.size();
            compiled.push_back(jobs[j]);
        }
    const std::vector<Bytes> built = CompileAll(prog.stage, source, fileName, compiled, options.jobs);
    std::vector<Bytes> codes(jobs.size());
    for (size_t j = 0; j < jobs.size(); j++)
        codes[j] = built[slot[codeOf[j]]];
    if (options.log && compiled.size() < jobs.size())
        options.log("  " + prog.stage + ": " + std::to_string(compiled.size()) + " of " + std::to_string(jobs.size()) +
                    " variants compiled (--only)");
    std::vector<bool> looked(codes.size(), false);
    std::vector<Bound> boundOf(codes.size());
    std::vector<Bytes> hashOf(codes.size());
    std::vector<std::shared_ptr<const Bytes>> shared(codes.size());   // each code once, for every block holding it
    for (size_t sid = 0; sid < staticValues.size(); sid++)
    {
        const auto& sv = staticValues[sid];
        // --s2fm: a pixel shader static combo with another value than the one built for keeps its lists but no code
        // (no pixel shader: it draws nothing), which halves the file for each such combo. Left out of the file
        // altogether, resourcecompiler refuses the materials ("Static combo not loaded!").
        bool leftOut = false;
        if (options.leaveOut && prog.stage == "ps")
            for (const auto& [n, v] : sv)
                for (const auto& [on, ov] : options.only)
                    leftOut = leftOut || (n == on && v != ov);
        vcs::StaticCombo sc;
        std::map<std::string, int> byCode;
        std::vector<int32_t> fileOffsets;
        int32_t cbSize = 0;
        std::vector<Field> allFields;
        // A static combo linked to a feature answers to the feature's name too.
        render_state::Combos combos;
        for (const auto& [n, v] : sv)
            combos[n] = v;
        for (size_t i = 0; i < out.statics.size(); i++)
            if (out.statics[i].feature >= 0)
                combos[(*prog.features)[out.statics[i].feature].name] = sv[i].second;
        render_state::Features featureIndex;
        for (size_t i = 0; i < prog.features->size(); i++)
            featureIndex[(*prog.features)[i].name] = static_cast<int>(i);
        render_state::Records records;
        try
        {
            sc.attributes = render_state::Attributes(prog.st->attributes, combos, featureIndex);
            if (prog.stage == "ps")
                records = render_state::Build(prog.st->renderStates, combos, featureIndex);
            else if (!prog.st->renderStates.empty())
                throw CompileError("RenderState belongs in the PS block");
        }
        catch (const render_state::RenderStateError& e)
        {
            throw CompileError(e.what());
        }
        // UiOnly combos: each one's flag takes a hidden constant of the combo's value, and while the value is 0 what
        // its feature shows is left out of the lists (the Material Editor shows only what the variant lists).
        std::map<int, int> flagVars;            // the flag parameter's variable: its constant's
        std::vector<std::string> hiddenFeatures;
        for (size_t i = 0; i < out.statics.size(); i++)
        {
            const ComboDef& c = out.statics[i];
            if (c.uiFlag.empty())
                continue;
            auto pv = prog.paramVars.find(c.uiFlag);
            if (pv == prog.paramVars.end())
            {
                if (prog.stage == "ps")
                    throw CompileError("UiOnly(" + c.uiFlag + "): the parameter is not declared");
                continue;
            }
            const int value = sv[i].second;
            flagVars[pv->second] = prog.Add("uiflag:" + c.uiFlag + ":" + std::to_string(value),
                                            ConstantVariable(c.uiFlag + "_" + std::to_string(value),
                                                             static_cast<float>(value)));
            if (value == 0)
                hiddenFeatures.push_back((*prog.features)[c.feature].name);
        }
        auto adjust = [&](std::vector<Field>& fields) {
            std::vector<Field> kept;
            for (Field f : fields)
            {
                auto u = prog.uiFeature.find(f.var);
                if (u != prog.uiFeature.end() &&
                    std::find(hiddenFeatures.begin(), hiddenFeatures.end(), u->second) != hiddenFeatures.end())
                    continue;
                auto fl = flagVars.find(f.var);
                if (fl != flagVars.end())
                    f.var = static_cast<uint16_t>(fl->second);
                kept.push_back(f);
            }
            fields = std::move(kept);
        };
        for (size_t did = 0; did < dynValues.size(); did++)
        {
            // What a compiled variant binds, worked out once: UiOnly combos share their code.
            const size_t job = codeIndex(sv) * dynValues.size() + did;
            if (!looked[job])
            {
                looked[job] = true;
                boundOf[job] = BindFields(prog, hlsl::Reflect(codes[job]), lib);
                if (prog.stage == "ps" && WritesNothing(codes[job]))
                    // A pixel shader with no output and no discard (a depth mode's) is stored as no code at all,
                    // as Valve's files have it: the engine then draws with no pixel shader bound.
                    codes[job].clear();
                hashOf[job] = vcs::Md5(codes[job]);
                shared[job] = std::make_shared<const Bytes>(codes[job]);
            }
            static const Bytes noCode;
            static const Bytes noCodeHash = vcs::Md5(noCode);
            const Bytes& code = leftOut ? noCode : codes[job];
            const std::shared_ptr<const Bytes> codeData = leftOut ? nullptr : shared[job];
            Bound bound = boundOf[job];
            // The render states given as variables (a record of them follows a feature): in the variant's state
            // part, with no slot, as Valve's files list them.
            for (const auto& s : records.variables)
            {
                std::string key = "renderstate:" + s.state + ":" + std::to_string(s.value) + ":";
                for (uint8_t byte : s.code)
                    key += std::to_string(byte) + ".";
                bound.state.push_back(MakeField(prog.Add(key, RenderStateVariable(s)), 0xFFFF));
            }
            adjust(bound.state);
            adjust(bound.constants);
            vcs::IndexArray arr;
            arr.firstRenderState = 0;
            arr.firstConstant = static_cast<int32_t>(bound.state.size());
            arr.fields = bound.state;
            arr.fields.insert(arr.fields.end(), bound.constants.begin(), bound.constants.end());
            for (const auto& f : arr.fields)
                if (std::find(allFields.begin(), allFields.end(), f) == allFields.end())
                    allFields.push_back(f);
            sc.dynamic.push_back(arr);
            if (bound.hasMaterial)
            {
                sc.cbSlots.emplace_back(static_cast<uint8_t>(bound.materialSlot), static_cast<uint8_t>(0));
                cbSize = std::max<int32_t>(cbSize, static_cast<int32_t>((bound.materialSize + 15) / 16 * 16));
            }
            else
                sc.cbSlots.emplace_back(static_cast<uint8_t>(255), static_cast<uint8_t>(255));
            if (prog.stage == "vs")
            {
                vcs::Signature sig;
                for (const auto& e : hlsl::Signature(code))
                {
                    std::string sem = e.semantic + (e.sv ? "" : std::to_string(e.index));
                    auto it = prog.st->semantics.find(sem);
                    if (it == prog.st->semantics.end())
                        it = prog.st->semantics.find(e.semantic);
                    if (it == prog.st->semantics.end())
                        throw CompileError("vs input " + sem + " has no Semantic() annotation");
                    sig.push_back({it->second.first, e.semantic, it->second.second, static_cast<int32_t>(e.index)});
                }
                // The engine reads the list sorted by D3D semantic and index, as Valve's files keep it; in
                // declaration order the shader is not used (the object falls back to a flat grey).
                std::stable_sort(sig.begin(), sig.end(), [](const auto& a, const auto& b) {
                    return a.d3dSemantic != b.d3dSemantic ? a.d3dSemantic < b.d3dSemantic : a.index < b.index;
                });
                auto pos = std::find(out.signatures.begin(), out.signatures.end(), sig);
                if (pos == out.signatures.end())
                {
                    out.signatures.push_back(sig);
                    pos = out.signatures.end() - 1;
                }
                sc.vsInputs.push_back(static_cast<int16_t>(pos - out.signatures.begin()));
            }
            const Bytes& hash = leftOut ? noCodeHash : hashOf[job];
            std::string key(hash.begin(), hash.end());
            if (!byCode.count(key))
            {
                byCode[key] = static_cast<int>(sc.files.size());
                // Where the file's record starts, counted from the file count before them (4 bytes, then the
                // glsl byte); each record is its size, the code and its MD5.
                int32_t offset = 5;
                for (const auto& x : sc.files)
                    offset += static_cast<int32_t>(4 + x.code().size() + 16);
                sc.files.push_back({codeData, hash});
                fileOffsets.push_back(offset);
            }
            vcs::State st;
            st.dyn = static_cast<int64_t>(did);
            st.file = byCode[key];
            st.fileOffset = fileOffsets[st.file];
            if (prog.stage == "ps")
            {
                st.has = records.has;
                st.rs = records.rs;
            }
            sc.states.push_back(st);
        }
        std::vector<Field> statePart, constPart;
        for (const auto& f : allFields)
        {
            bool inState = false;
            for (const auto& a : sc.dynamic)
                for (int32_t k = 0; k < a.firstConstant && !inState; k++)
                    inState = a.fields[k] == f;
            (inState ? statePart : constPart).push_back(f);
        }
        // A material buffer holding a value the engine sets (g_bIsSFM...) is not static: marked static, the engine
        // never writes those values and they read 0. Every such block in the game's files has 0 here.
        sc.staticCB = 1;
        for (const auto& f : constPart)
            if (prog.variables[f.var].source == 1)
                sc.staticCB = 0;
        sc.all.firstRenderState = 0;
        sc.all.firstConstant = static_cast<int32_t>(statePart.size());
        sc.all.fields = statePart;
        sc.all.fields.insert(sc.all.fields.end(), constPart.begin(), constPart.end());
        sc.cbSize = cbSize;
        sc.glsl = 0;
        int64_t cid = 0, mul = 1;
        for (size_t i = 0; i < out.statics.size(); i++)
        {
            cid += (sv[i].second - out.statics[i].min) * mul;
            mul *= out.statics[i].max - out.statics[i].min + 1;
        }
        if (options.log)
            options.log("  " + prog.stage + " static " + Describe(sv) + ": " + std::to_string(sc.states.size()) +
                        " variants, " + std::to_string(sc.files.size()) + " distinct");
        out.combos.push_back(std::move(sc));
        out.ids.push_back(cid);
    }
    SortVariables(prog, out.combos);
    return out;
}

vcs::File ProgramFile(Program& prog, Built& built, const Library& lib, const std::map<std::string, Bytes>& hashes)
{
    vcs::File f;
    f.program = prog.stage;
    // A constant buffer variable's bits is the place of its description in the file's list: the engine finds the
    // buffer to bind through it.
    for (auto& v : prog.variables)
    {
        auto cb = lib.cbuffers.find(vcs::Text(v.name));
        if (v.reg == REG_CBUFFER && cb != lib.cbuffers.end())
        {
            v.bits = static_cast<int32_t>(f.cbuffers.size());
            f.cbuffers.push_back(cb->second);
        }
    }
    f.version = 64;
    f.flags = 0;
    f.programHashes = {hashes.at(prog.stage)};
    f.variableHash = lib.variableHash;
    f.variableSourceMax = lib.variableSourceMax;
    for (const auto& c : built.statics)
        f.statics.push_back(ComboRecord(c, 1));
    for (const auto& c : built.dynamics)
        f.dynamics.push_back(ComboRecord(c, 2));
    f.variables = prog.variables;
    f.channels = prog.channels;
    if (prog.stage == "vs")
        f.signatures = built.signatures;
    f.blocks.resize(built.combos.size());
    std::atomic<size_t> next{0};
    auto work = [&]() {
        for (size_t i; (i = next++) < built.combos.size();)
        {
            f.blocks[i].id = built.ids[i];
            f.blocks[i].stored = vcs::PackBlock(vcs::WriteStaticCombo(built.combos[i], prog.stage), vcs::Pack::Lzma);
            built.combos[i] = vcs::StaticCombo();
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < std::max(1u, std::thread::hardware_concurrency()); t++)
        threads.emplace_back(work);
    work();
    for (auto& t : threads)
        t.join();
    return f;
}

vcs::File FeaturesFile(const vfx::Shader& shader, std::map<std::string, Program>& programs,
                       const std::map<std::string, Bytes>& hashes, const Library& lib)
{
    vcs::File f;
    f.program = "features";
    std::vector<std::pair<std::string, std::vector<int32_t>>> keys;     // (name, reg + intDefs) per variable
    for (const char* stage : {"vs", "ps"})
    {
        Program& prog = programs[stage];
        std::map<int, int> remap;
        for (size_t i = 0; i < prog.variables.size(); i++)
        {
            const auto& v = prog.variables[i];
            std::vector<int32_t> key{v.reg, v.intDefs[0], v.intDefs[1], v.intDefs[2], v.intDefs[3]};
            std::pair<std::string, std::vector<int32_t>> k{vcs::Text(v.name), key};
            auto pos = std::find(keys.begin(), keys.end(), k);
            if (pos == keys.end())
            {
                keys.push_back(k);
                f.variables.push_back(v);
                pos = keys.end() - 1;
            }
            remap[static_cast<int>(i)] = static_cast<int>(pos - keys.begin());
        }
        for (const auto& c : prog.channels)
        {
            vcs::Channel c2 = c;
            for (auto& i : c2.inputs)
                i = i >= 0 ? remap[i] : -1;
            f.channels.push_back(c2);
        }
        // Texture variables point at the features file's own channel list.
        for (size_t i = 0; i < prog.variables.size(); i++)
        {
            const auto& v = prog.variables[i];
            if (v.reg == REG_TEXTURE && v.channels)
            {
                int base = static_cast<int>(f.channels.size() - prog.channels.size());
                auto& target = f.variables[remap[static_cast<int>(i)]];
                for (int k = 0; k < 4; k++)
                    target.channelInfo[k] = v.channelInfo[k] >= 0 ? v.channelInfo[k] + base : -1;
            }
        }
    }
    for (const auto& feat : shader.features)
    {
        vcs::Combo c;
        c.name = vcs::Fixed(feat.name, 64);
        c.alias = vcs::Fixed(feat.group, 64);
        c.type = 0;
        c.min = feat.min;
        c.max = feat.max;
        c.source = 0;
        c.feature = -1;
        c.states = feat.states;
        f.statics.push_back(c);
    }
    f.version = 64;
    f.flags = 0;
    f.headerVersion = 1;
    f.description = shader.description;
    f.dev = 0;
    f.available = {1, 1, 1, 0, 0, 0, 0};
    for (const auto& m : shader.modes)
    {
        vcs::Mode mode;
        mode.name = vcs::Fixed(m.name, 64);
        mode.fallback = vcs::Fixed(m.fallback, 64);
        for (const auto& [c, v] : m.combos)
            mode.combos.emplace_back(vcs::Fixed(c, 64), v);
        f.modes.push_back(mode);
    }
    f.programHashes = {hashes.at("features"), hashes.at("vs"), hashes.at("ps")};
    for (int i = 0; i < 4; i++)
        f.programHashes.push_back(Bytes(16, 0));
    f.variableHash = lib.variableHash;
    f.variableSourceMax = lib.variableSourceMax;
    return f;
}
} // namespace

std::pair<std::string, std::map<std::string, Bytes>> CompileShader(const CompileOptions& options, const Library& lib)
{
    vfx::Shader shader;
    try
    {
        shader = vfx::Parse(options.source, options.includeDirs);
    }
    catch (const vfx::VfxError& e)
    {
        throw CompileError(e.what());
    }
    if (!options.name.empty())
        shader.name = options.name;
    std::filesystem::path includeDir = options.includeDirs.empty() ? options.source.parent_path() : options.includeDirs[0];
    std::map<std::string, Program> programs;
    std::map<std::string, Built> built;
    for (const char* stage : {"vs", "ps"})
    {
        Program& prog = programs[stage];
        prog.stage = stage;
        prog.st = &shader.stages.at(stage);
        prog.features = &shader.features;
        prog.Declare();
        built[stage] = CompileProgram(shader.name, prog, lib, options, includeDir);
    }
    Bytes src = vpk::ReadFile(options.source);
    std::map<std::string, Bytes> hashes;
    for (const char* k : {"features", "vs", "ps"})
    {
        Bytes b = src;
        b.insert(b.end(), k, k + std::strlen(k));
        hashes[k] = vcs::Md5(b);
    }
    std::map<std::string, Bytes> out;
    for (const char* stage : {"vs", "ps"})
        out[stage] = vcs::Write(ProgramFile(programs[stage], built[stage], lib, hashes), vcs::Pack::Keep);
    out["features"] = vcs::Write(FeaturesFile(shader, programs, hashes, lib), vcs::Pack::Lzma);
    return {shader.name, out};
}
