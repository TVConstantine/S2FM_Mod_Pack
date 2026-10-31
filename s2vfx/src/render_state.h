// A pixel shader state's render state records (rasterizer 16 bytes, depth stencil 20, blend 75) and a static combo's
// attributes, from a shader's RenderState(...) and Bool/Int/FloatAttribute(...) statements. A port of
// python/render_state.py; the values and layouts are described there and in docs/format_v64.md.
//
// A condition (S_A ? x : y) may name features that have no static combo: the state or attribute then follows the
// material's feature through an expression the engine works out, as Valve's files do it (vr_complex's DstBlend and
// DoNotCastShadows), with no static combo and no code of its own.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "vcs.h"

namespace render_state
{

struct RenderStateError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

using Combos = std::map<std::string, int>;     // static combo (and feature) name: value
using Features = std::map<std::string, int>;   // a feature's name: its index in the features file

// A render state given as a variable instead of in a record, as Valve's translucent variants have their blend:
// a literal (code empty, the value in value) or an expression of the features (code). The variable's type, the
// state's id and its context (rasterizer 128, depth stencil 256, blend 512).
struct StateVariable
{
    std::string state;
    Bytes code;
    double value = 0;
    int type = 5, bits = 0, context = 0, elements = 1;
};

struct Records
{
    std::array<uint8_t, 3> has{1, 1, 1};
    std::array<Bytes, 3> rs;
    // A record any of whose states follows a feature is left out, and all of its states are given here instead.
    std::vector<StateVariable> variables;
};

Records Build(const std::vector<std::pair<std::string, std::string>>& statements, const Combos& combos,
              const Features& features = {});
std::vector<vcs::Attribute> Attributes(const std::vector<std::tuple<std::string, std::string, std::string>>& statements,
                                       const Combos& combos, const Features& features = {});

} // namespace render_state
