// Reads a .vfx shader source (the working subset of Valve's VFX syntax described in python/vfx.py) into a
// description the compiler uses. A line for line port of python/vfx.py.
#pragma once

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace vfx
{

struct VfxError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Call
{
    std::string name;
    std::vector<std::string> args;
};

struct StaticComboDecl
{
    std::string name;
    std::string feature;            // F_... when linked to a feature
    int min = 0, max = 0;
    // UiOnly(param): the combo only says what the Material Editor shows. The code is not compiled once per value;
    // param (a float the code reads) is given the combo's value instead, and what is marked UiFeature(F_...) with
    // the combo's feature is left out of the variant's lists while the value is 0, which hides it.
    std::string uiFlag;
};

struct DynamicComboDecl
{
    std::string name;
    int min = 0, max = 0;
};

struct InputTexture
{
    std::string name, colorSpace = "Linear";
    int bits = 8;
    std::string processing, suffix, group;
    float defaults[4] = {1.0f, 1.0f, 1.0f, 0.0f};
};

struct Annotated
{
    std::string type, name;
    std::vector<Call> annotations;
};

struct Stage
{
    std::vector<StaticComboDecl> statics;
    std::vector<DynamicComboDecl> dynamics;
    std::vector<InputTexture> inputs;
    std::vector<Annotated> params, textures, samplers;
    std::map<std::string, std::pair<std::string, std::string>> semantics;   // HLSL semantic: (member, stream)
    std::vector<std::pair<std::string, std::string>> renderStates;          // (state, value expression)
    std::vector<std::tuple<std::string, std::string, std::string>> attributes;  // (kind, name, value expression)
    std::string hlsl;
};

struct Feature
{
    std::string name;
    int min = 0, max = 0;
    std::string group;
    std::vector<std::string> states;    // the value names the Material Editor lists: 0..2 (0="None", 1="One", ...)
};

struct ModeDecl
{
    std::string name, fallback;
    std::vector<std::pair<std::string, int>> combos;
};

struct Shader
{
    std::string name, description;
    std::vector<ModeDecl> modes;
    std::vector<Feature> features;
    std::map<std::string, Stage> stages;    // "vs", "ps"
};

std::vector<std::string> SplitArgs(const std::string& s);
std::vector<Call> Calls(const std::string& s);
std::string Unquote(const std::string& s);
std::pair<int, int> ParseRange(const std::string& s);

Shader Parse(const std::filesystem::path& path, const std::vector<std::filesystem::path>& includeDirs);

} // namespace vfx
