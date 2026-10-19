// The panel's C API (rs_core_api.h, plan task 9). Called on Qt's thread; every call holds rs::api_mutex(),
// which the render thread holds for a whole stage call, so the runtime is never read or changed half way
// through a frame. ReShade's own effect runtime API does the work; a change has S2FM draw the view again.
#define RS_CORE_EXPORTS
#include "rs_core_api.h"
#include "rs_game.h"
#include "rs_assign.h"
#include "rs_runtime.h"

#include "runtime.hpp"
#include "ini_file.hpp"

#include <windows.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using reshade::api::effect_technique;
using reshade::api::effect_uniform_variable;

namespace {

void Copy(char* out, int size, const std::string& s) {
    if (out && size > 0) strncpy_s(out, size, s.c_str(), _TRUNCATE);
}

// The techniques in the preset's order.
std::vector<effect_technique> Techniques(reshade::runtime* rt) {
    std::vector<effect_technique> out;
    if (!rt) return out;
    rt->enumerate_techniques(nullptr, [](reshade::api::effect_runtime*, effect_technique t, void* user) {
        static_cast<std::vector<effect_technique>*>(user)->push_back(t);
    }, &out);
    return out;
}

bool Technique(int i, reshade::runtime** rt, effect_technique* t) {
    *rt = rs::core_runtime();
    if (!*rt || rs::core_loading()) return false;
    const std::vector<effect_technique> all = Techniques(*rt);
    if (i < 0 || i >= (int)all.size()) return false;
    *t = all[(size_t)i];
    return true;
}

std::string EffectOf(reshade::runtime* rt, effect_technique t) {
    char name[260] = "";
    size_t n = sizeof(name);
    rt->get_technique_effect_name(t, name, &n);
    return name;
}

std::string TechniqueName(reshade::runtime* rt, effect_technique t) {
    char name[260] = "";
    size_t n = sizeof(name);
    rt->get_technique_name(t, name, &n);
    return name;
}

std::string UniformString(reshade::runtime* rt, effect_uniform_variable v, const char* annotation) {
    char b[4096] = "";
    size_t n = sizeof(b);
    if (!static_cast<reshade::api::effect_runtime*>(rt)->get_annotation_string_from_uniform_variable(v, annotation, b, &n)) return std::string();
    return std::string(b, n > 0 && b[n - 1] == 0 ? n - 1 : n);
}

// The settings of an effect the panel shows: not the ones the runtime fills itself (a "source"), nor hidden.
std::vector<effect_uniform_variable> Uniforms(reshade::runtime* rt, const std::string& effect) {
    std::vector<effect_uniform_variable> all, out;
    rt->enumerate_uniform_variables(effect.c_str(), [](reshade::api::effect_runtime*, effect_uniform_variable v, void* user) {
        static_cast<std::vector<effect_uniform_variable>*>(user)->push_back(v);
    }, &all);
    for (effect_uniform_variable v : all) {
        if (!UniformString(rt, v, "source").empty()) continue;
        bool hidden = false;
        static_cast<reshade::api::effect_runtime*>(rt)->get_annotation_bool_from_uniform_variable(v, "hidden", &hidden, 1);
        if (!hidden) out.push_back(v);
    }
    return out;
}

bool Uniform(int i, int u, reshade::runtime** rt, effect_uniform_variable* v) {
    effect_technique t = {};
    if (!Technique(i, rt, &t)) return false;
    const std::vector<effect_uniform_variable> all = Uniforms(*rt, EffectOf(*rt, t));
    if (u < 0 || u >= (int)all.size()) return false;
    *v = all[(size_t)u];
    return true;
}

// The runtime's state written into the preset's copy, so a reload or a new effect keeps what was changed. Not while
// it is loading: it has no techniques then, and an empty list would make the next runtime compile every effect in
// the folder. Nothing can have been changed while loading, so the copy is right as it is.
bool KeepState(reshade::runtime* rt) {
    if (!rt || rs::core_loading()) return false;
    rt->save_current_preset();
    reshade::ini_file::flush_cache(rs::game_preset_copy());
    return true;
}

// The effect search paths of the runtime's config (run.ini), "\**" meaning the folder and below.
std::vector<std::pair<fs::path, bool>> SearchPaths() {
    std::vector<std::pair<fs::path, bool>> out;
    std::ifstream f(rs::game_bin() / "s2fm_reshade" / "run.ini");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 18, "EffectSearchPaths=") != 0) continue;
        std::string list = line.substr(18);
        while (!list.empty() && (list.back() == '\r' || list.back() == '\n')) list.pop_back();
        size_t start = 0;
        while (start <= list.size()) {
            size_t end = list.find(',', start);
            if (end == std::string::npos) end = list.size();
            std::string p = list.substr(start, end - start);
            const bool below = p.size() >= 3 && p.compare(p.size() - 3, 3, "\\**") == 0;
            if (below) p.resize(p.size() - 3);
            if (!p.empty()) out.emplace_back(fs::u8path(p), below);
            start = end + 1;
        }
    }
    return out;
}

fs::path FindEffect(const std::string& file) {
    std::error_code ec;
    for (const auto& sp : SearchPaths()) {
        if (sp.second) {
            for (fs::recursive_directory_iterator it(sp.first, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
                if (it->path().filename().u8string() == file) return it->path();
        } else if (fs::exists(sp.first / fs::u8path(file), ec)) {
            return sp.first / fs::u8path(file);
        }
    }
    return fs::path();
}

}  // namespace

RS_API int RsCore_Status(RsStatus* out) {
    if (!out) return 0;
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    memset(out, 0, sizeof(*out));
    out->running = rs::core_ready() ? 1 : 0;
    out->loading = rs::core_loading() ? 1 : 0;
    out->effectsOn = rs::game_effects_on() ? 1 : 0;
    out->width = rs::core_width();
    out->height = rs::core_height();
    for (int s = 0; s < 3; ++s) out->stageMs[s] = (float)rs::core_stage_ms(s);
    if (reshade::runtime* rt = rs::core_runtime()) {
        if (!out->loading) out->techniques = (int)Techniques(rt).size();
        const std::string errors = rs::core_errors();
        for (size_t at = 0; (at = errors.find("(FAILED)", at)) != std::string::npos; ++at) ++out->failed;
    }
    Copy(out->preset, sizeof(out->preset), rs::game_preset_source());
    Copy(out->message, sizeof(out->message), !rs::game_started() ? "the core is not started"
         : rs::game_preset_source().empty() ? "no preset" : out->loading ? "compiling effects" : "");
    return 1;
}

RS_API int RsCore_EffectsOn(int on) {
    if (on >= 0) {
        rs::game_set_effects_on(on != 0);
        rs::game_redraw();
    }
    return rs::game_effects_on() ? 1 : 0;
}

RS_API int RsCore_LoadPreset(const char* path) {
    if (!rs::game_started()) return 0;
    rs::game_use_preset(path ? path : "");
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_SavePreset(const char* path) {
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    reshade::runtime* rt = rs::core_runtime();
    const std::string target = path && *path ? std::string(path) : rs::game_preset_source();
    // Not while loading: the preset itself would be written with no techniques.
    if (!rt || target.empty() || !KeepState(rt)) return 0;
    std::error_code ec;
    fs::copy_file(rs::game_preset_copy(), fs::u8path(target), fs::copy_options::overwrite_existing, ec);
    return ec ? 0 : 1;
}

RS_API int RsCore_ListPresets(char* out, int size) {
    std::string list;
    std::error_code ec;
    for (fs::directory_iterator it(rs::game_bin(), ec), end; it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (p.extension() != ".ini") continue;
        const std::string name = p.filename().u8string();
        if (name.compare(0, 7, "ReShade") == 0 && name != "ReShadePreset.ini") continue;   // ReShade's own configs
        std::ifstream f(p);
        std::string line;
        bool preset = false;
        for (int n = 0; n < 400 && std::getline(f, line); ++n)
            if (line.compare(0, 11, "Techniques=") == 0) { preset = true; break; }
        if (preset) list += p.u8string() + "\n";
    }
    Copy(out, size, list);
    return (int)list.size();
}

RS_API int RsCore_Reload() {
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        KeepState(rs::core_runtime());
    }
    rs::game_reload();
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_TechniqueInfo(int i, RsTechniqueInfo* out) {
    if (!out) return 0;
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    memset(out, 0, sizeof(*out));
    reshade::runtime* rt = nullptr;
    effect_technique t = {};
    if (!Technique(i, &rt, &t)) return 0;
    const reshade::runtime::s2fm_technique_facts facts = rt->s2fm_technique_info(t);
    Copy(out->name, sizeof(out->name), facts.name);
    Copy(out->effect, sizeof(out->effect), facts.file);
    out->enabled = rt->get_technique_state(t) ? 1 : 0;
    out->readsDepth = facts.reads_depth ? 1 : 0;
    bool hand = false;
    out->stage = rs::game_stage(rt->s2fm_technique_index(t), &out->autoStage, &hand);
    out->overridden = hand ? 1 : 0;
    return 1;
}

RS_API int RsCore_SetTechniqueEnabled(int i, int on) {
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        reshade::runtime* rt = nullptr;
        effect_technique t = {};
        if (!Technique(i, &rt, &t)) return 0;
        rt->set_technique_state(t, on != 0);
    }
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_SetTechniqueStage(int i, int stage) {
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        reshade::runtime* rt = nullptr;
        effect_technique t = {};
        if (!Technique(i, &rt, &t)) return 0;
        name = TechniqueName(rt, t);
    }
    rs::game_set_stage(name, stage < 0 || stage > 2 ? -1 : stage);
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_MoveTechnique(int i, int to) {
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        reshade::runtime* rt = rs::core_runtime();
        if (!rt || rs::core_loading()) return 0;
        std::vector<effect_technique> all = Techniques(rt);
        if (i < 0 || i >= (int)all.size() || to < 0 || to >= (int)all.size()) return 0;
        const effect_technique t = all[(size_t)i];
        all.erase(all.begin() + i);
        all.insert(all.begin() + to, t);
        rt->reorder_techniques(all.size(), all.data());
    }
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_EffectErrors(int i, char* out, int size) {
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    reshade::runtime* rt = nullptr;
    effect_technique t = {};
    if (!Technique(i, &rt, &t)) return 0;
    const std::string e = rt->s2fm_effect_errors(EffectOf(rt, t));
    Copy(out, size, e);
    return (int)e.size();
}

RS_API int RsCore_UniformCount(int i) {
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    reshade::runtime* rt = nullptr;
    effect_technique t = {};
    if (!Technique(i, &rt, &t)) return 0;
    return (int)Uniforms(rt, EffectOf(rt, t)).size();
}

RS_API int RsCore_UniformInfo(int i, int u, RsUniformInfo* out) {
    if (!out) return 0;
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    memset(out, 0, sizeof(*out));
    reshade::runtime* rt = nullptr;
    effect_uniform_variable v = {};
    if (!Uniform(i, u, &rt, &v)) return 0;
    char name[256] = "";
    size_t n = sizeof(name);
    static_cast<reshade::api::effect_runtime*>(rt)->get_uniform_variable_name(v, name, &n);
    Copy(out->name, sizeof(out->name), name);
    const std::string label = UniformString(rt, v, "ui_label");
    Copy(out->label, sizeof(out->label), label.empty() ? std::string(name) : label);
    Copy(out->category, sizeof(out->category), UniformString(rt, v, "ui_category"));
    Copy(out->tooltip, sizeof(out->tooltip), UniformString(rt, v, "ui_tooltip"));
    Copy(out->uiType, sizeof(out->uiType), UniformString(rt, v, "ui_type"));
    Copy(out->units, sizeof(out->units), UniformString(rt, v, "ui_units"));
    reshade::api::format base = reshade::api::format::unknown;
    uint32_t rows = 1, columns = 1, array = 0;
    static_cast<reshade::api::effect_runtime*>(rt)->get_uniform_variable_type(v, &base, &rows, &columns, &array);
    out->baseType = base == reshade::api::format::r32_typeless ? 0
                  : (base == reshade::api::format::r32_sint || base == reshade::api::format::r16_sint) ? 1
                  : (base == reshade::api::format::r32_uint || base == reshade::api::format::r16_uint) ? 2 : 3;
    out->components = (int)(rows * columns);
    if (out->components < 1) out->components = 1;
    if (out->components > 16) out->components = 16;
    out->uiMin = 0.0f;
    out->uiMax = out->baseType == 3 ? 1.0f : 100.0f;
    out->uiStep = 0.0f;
    static_cast<reshade::api::effect_runtime*>(rt)->get_annotation_float_from_uniform_variable(v, "ui_min", &out->uiMin, 1);
    static_cast<reshade::api::effect_runtime*>(rt)->get_annotation_float_from_uniform_variable(v, "ui_max", &out->uiMax, 1);
    static_cast<reshade::api::effect_runtime*>(rt)->get_annotation_float_from_uniform_variable(v, "ui_step", &out->uiStep, 1);
    char items[2048] = "";
    size_t itemsSize = sizeof(items);
    if (static_cast<reshade::api::effect_runtime*>(rt)->get_annotation_string_from_uniform_variable(v, "ui_items", items, &itemsSize)) {
        memcpy(out->items, items, itemsSize < sizeof(out->items) ? itemsSize : sizeof(out->items) - 1);
        for (size_t k = 0; k < itemsSize && k < sizeof(out->items); ++k)
            if (out->items[k] == 0 && k > 0 && out->items[k - 1] != 0) ++out->itemCount;
    }
    bool closed = false;
    static_cast<reshade::api::effect_runtime*>(rt)->get_annotation_bool_from_uniform_variable(v, "ui_category_closed", &closed, 1);
    out->categoryClosed = closed ? 1 : 0;
    return 1;
}

RS_API int RsCore_GetUniform(int i, int u, float* values, int count) {
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    reshade::runtime* rt = nullptr;
    effect_uniform_variable v = {};
    if (!Uniform(i, u, &rt, &v) || !values || count <= 0) return 0;
    reshade::api::format base = reshade::api::format::unknown;
    static_cast<reshade::api::effect_runtime*>(rt)->get_uniform_variable_type(v, &base);
    if (base == reshade::api::format::r32_float || base == reshade::api::format::r16_float) {
        static_cast<reshade::api::effect_runtime*>(rt)->get_uniform_value_float(v, values, (size_t)count);
    } else if (base == reshade::api::format::r32_typeless) {
        bool b[16] = {};
        static_cast<reshade::api::effect_runtime*>(rt)->get_uniform_value_bool(v, b, (size_t)(count > 16 ? 16 : count));
        for (int k = 0; k < count && k < 16; ++k) values[k] = b[k] ? 1.0f : 0.0f;
    } else {
        int32_t n[16] = {};
        static_cast<reshade::api::effect_runtime*>(rt)->get_uniform_value_int(v, n, (size_t)(count > 16 ? 16 : count));
        for (int k = 0; k < count && k < 16; ++k) values[k] = (float)n[k];
    }
    return 1;
}

RS_API int RsCore_SetUniform(int i, int u, const float* values, int count) {
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        reshade::runtime* rt = nullptr;
        effect_uniform_variable v = {};
        if (!Uniform(i, u, &rt, &v) || !values || count <= 0) return 0;
        reshade::api::format base = reshade::api::format::unknown;
        static_cast<reshade::api::effect_runtime*>(rt)->get_uniform_variable_type(v, &base);
        if (base == reshade::api::format::r32_float || base == reshade::api::format::r16_float) {
            static_cast<reshade::api::effect_runtime*>(rt)->set_uniform_value_float(v, values, (size_t)count);
        } else if (base == reshade::api::format::r32_typeless) {
            bool b[16] = {};
            for (int k = 0; k < count && k < 16; ++k) b[k] = values[k] != 0.0f;
            static_cast<reshade::api::effect_runtime*>(rt)->set_uniform_value_bool(v, b, (size_t)(count > 16 ? 16 : count));
        } else {
            int32_t n[16] = {};
            for (int k = 0; k < count && k < 16; ++k) n[k] = (int32_t)(values[k] < 0 ? values[k] - 0.5f : values[k] + 0.5f);
            static_cast<reshade::api::effect_runtime*>(rt)->set_uniform_value_int(v, n, (size_t)(count > 16 ? 16 : count));
        }
    }
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_ResetUniform(int i, int u) {
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        reshade::runtime* rt = nullptr;
        effect_uniform_variable v = {};
        if (!Uniform(i, u, &rt, &v)) return 0;
        static_cast<reshade::api::effect_runtime*>(rt)->reset_uniform_value(v);
    }
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_DefinitionCount(int i) {
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    reshade::runtime* rt = nullptr;
    effect_technique t = {};
    if (!Technique(i, &rt, &t)) return 0;
    return (int)rt->s2fm_effect_definitions(EffectOf(rt, t)).size();
}

RS_API int RsCore_Definition(int i, int d, char* name, int nameSize, char* value, int valueSize) {
    std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
    reshade::runtime* rt = nullptr;
    effect_technique t = {};
    if (!Technique(i, &rt, &t)) return 0;
    const std::string effect = EffectOf(rt, t);
    const auto defs = rt->s2fm_effect_definitions(effect);
    if (d < 0 || d >= (int)defs.size()) return 0;
    Copy(name, nameSize, defs[(size_t)d].first);
    char current[512] = "";
    size_t n = sizeof(current);
    if (rt->get_preprocessor_definition_for_effect(effect.c_str(), defs[(size_t)d].first.c_str(), current, &n))
        Copy(value, valueSize, current);
    else
        Copy(value, valueSize, defs[(size_t)d].second);
    return 1;
}

RS_API int RsCore_SetDefinition(int i, const char* name, const char* value) {
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        reshade::runtime* rt = nullptr;
        effect_technique t = {};
        if (!Technique(i, &rt, &t) || !name || !*name) return 0;
        rt->set_preprocessor_definition_for_effect(EffectOf(rt, t).c_str(), name, value ? value : "");
        rs::game_stages_dirty();
    }
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_ListEffects(char* out, int size) {
    // Each file once, from the first search path that has it (as FindEffect finds it), with its collection: the
    // first folder below the search path it is in (qUINT, SweetFX, ...), the search path's own name for a file at
    // its top, "S2FM" for the package's own folder.
    std::map<std::string, std::string> names;
    std::error_code ec;
    for (const auto& sp : SearchPaths()) {
        const std::string root = sp.first.parent_path().filename().u8string() == "s2fm_reshade"
                                     ? std::string("S2FM") : sp.first.filename().u8string();
        if (sp.second) {
            for (fs::recursive_directory_iterator it(sp.first, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                if (it->path().extension() != ".fx") continue;
                const fs::path rel = it->path().lexically_relative(sp.first);
                const std::string collection = std::distance(rel.begin(), rel.end()) > 1 ? rel.begin()->u8string() : root;
                names.emplace(it->path().filename().u8string(), collection);
            }
        } else {
            for (fs::directory_iterator it(sp.first, ec), end; it != end; it.increment(ec))
                if (it->path().extension() == ".fx") names.emplace(it->path().filename().u8string(), root);
        }
    }
    std::string list;
    for (const auto& n : names) list += n.first + "\t" + n.second + "\n";
    Copy(out, size, list);
    return (int)list.size();
}

RS_API int RsCore_AddEffect(const char* file) {
    if (!file || !*file) return 0;
    const fs::path path = FindEffect(file);
    if (path.empty()) return 0;
    // Its techniques, from the source (ReShade loads only what the preset names, so the runtime has not
    // read this file): "technique Name" at the start of a line.
    std::ifstream f(path);
    std::string line;
    std::vector<std::string> found;
    const std::regex re("^\\s*technique\\s+([A-Za-z_][A-Za-z0-9_]*)");
    std::smatch m;
    while (std::getline(f, line))
        if (std::regex_search(line, m, re)) found.push_back(m[1].str() + "@" + std::string(file));
    if (found.empty()) return 0;
    {
        std::lock_guard<std::recursive_mutex> lk(rs::api_mutex());
        KeepState(rs::core_runtime());
        reshade::ini_file& ini = reshade::ini_file::load_cache(rs::game_preset_copy());
        std::vector<std::string> techniques, sorting;
        ini.get({}, "Techniques", techniques);
        ini.get({}, "TechniqueSorting", sorting);
        for (const std::string& t : found) {
            if (std::find(techniques.begin(), techniques.end(), t) == techniques.end()) techniques.push_back(t);
            if (std::find(sorting.begin(), sorting.end(), t) == sorting.end()) sorting.push_back(t);
        }
        ini.set({}, "Techniques", std::move(techniques));
        ini.set({}, "TechniqueSorting", std::move(sorting));
        reshade::ini_file::flush_cache(rs::game_preset_copy());
    }
    rs::game_reload();
    rs::game_redraw();
    return (int)found.size();
}
