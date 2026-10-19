// The core in the game (plan tasks 6 and 10): RsCore_Start, called by the proxy (or the panel) with the game's
// bin\win64 folder, registers the stage callback with dxgi.dll. The runtime is made on the first stage call,
// on the engine's device and immediate context, at the picture's size.
//
// The runtime is never handed one of the user's files, because it writes back what it reads:
//   bin\win64\s2fm_reshade\run.ini     its config, made from ReShade.ini's [GENERAL] (paths made absolute)
//   bin\win64\s2fm_reshade\preset.ini  a copy of the preset in use
// and the core's own memory (task 13):
//   bin\win64\s2fm_reshade\state.ini   [GENERAL] Enabled (some session has a preset: the proxy loads the core
//                                     at start), EffectsOn; [SESSIONS] session name = preset, for each session
// ReShade.ini is read only; a preset is written back only by the panel's Save (rs_api.cpp).
#define RS_CORE_EXPORTS
#include "rs_core_api.h"
#include "rs_game.h"
#include "rs_assign.h"
#include "rs_runtime.h"
#include "rs_stage_io.h"
#include "rs_stage_api.h"
#include "runtime.hpp"
#include "ini_file.hpp"
#include "dll_log.hpp"

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::recursive_mutex g_mutex;
fs::path g_bin;                 // game\bin\win64
fs::path g_runDir;              // game\bin\win64\s2fm_reshade
fs::path g_runIni, g_runPreset;
std::vector<std::pair<std::string, std::string>> g_definitions;
bool g_started = false;
bool g_effectsOn = true;
HMODULE g_dxgi = nullptr;

// Test words (the proxy's switch file, s2fm_reshade_debug.txt beside the game's DLLs, read here too): preset=<path>
// loads a copy of that preset, stage=A|B|C puts every technique at one stage.
const char* const kSwitch = "s2fm_reshade_debug.txt";
std::string g_switchLine;
ULONGLONG g_switchLook = 0;
std::string g_presetSource;     // the preset the copy was made from
bool g_presetFromSwitch = false;
bool g_presetEmpty = true;      // the copy names no technique (worked out when it is made or reloaded)
// The presets by session (state.ini), and the session open now.
std::map<std::string, std::string> g_sessions;
std::string g_session;
ULONGLONG g_sessionLook = 0;
fs::file_time_type g_stateTime;   // state.ini's time when last read or written here
int g_testStage = -1;
bool g_reinit = false;
// What S2FM drew last, for the tick (RsCore_Tick): when a stage was last called, and whether a render's.
std::atomic<ULONGLONG> g_lastStageAt{ 0 };
std::atomic<bool> g_lastRendering{ false };
ULONGLONG g_madeAt = 0;         // when the runtime was last made
bool g_wasLoading = false;      // for the line that says the effects are in

// Stages by the runtime's technique index: in use, the rules', set by hand; worked out once loading is done.
std::vector<int> g_stageOf, g_autoOf;
std::vector<bool> g_handOf;
bool g_stagesKnown = false;
std::map<std::string, int> g_hand;   // technique name -> stage, from the copy's [S2FM]
// A picture of another size: the runtime is made again only when it lasts (another viewport would thrash it).
uint32_t g_otherW = 0, g_otherH = 0;
int g_otherCount = 0;

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// A section of an ini file as key/value pairs, in order.
std::vector<std::pair<std::string, std::string>> IniSection(const fs::path& p, const std::string& section) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(p);
    std::string line, cur;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { cur = line.substr(1, line.find(']') - 1); continue; }
        if (cur != section) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    return out;
}

std::string IniValue(const std::vector<std::pair<std::string, std::string>>& kv, const char* key, const char* fallback = "") {
    for (const auto& p : kv) if (p.first == key) return p.second;
    return fallback;
}

std::vector<std::string> SplitCommas(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        const std::string item = Trim(s.substr(start, end - start));
        if (!item.empty()) out.push_back(item);
        start = end + 1;
    }
    return out;
}

// A search path from ReShade.ini, made absolute against bin\win64 (ReShade would resolve it against the
// folder its own DLL is in, which for us is bin\win64\tools). A trailing "\**" (search below) is kept.
std::string Absolute(const std::string& path) {
    std::string base = path, tail;
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "\\**") == 0) { tail = "\\**"; base.resize(base.size() - 3); }
    fs::path p = fs::u8path(base);
    if (p.is_relative()) p = g_bin / p;
    return fs::weakly_canonical(p).u8string() + tail;
}

// The definitions every effect gets: the user's, with the depth input ones forced to what the core hands over.
void MakeDefinitions(const std::string& users) {
    g_definitions.clear();
    for (const std::string& item : SplitCommas(users)) {
        const size_t eq = item.find('=');
        g_definitions.emplace_back(item.substr(0, eq), eq == std::string::npos ? "1" : item.substr(eq + 1));
    }
    const std::pair<const char*, const char*> forced[] = {
        { "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0" }, { "RESHADE_DEPTH_INPUT_IS_REVERSED", "0" },
        { "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0" }, { "RESHADE_DEPTH_INPUT_X_SCALE", "1" },
        { "RESHADE_DEPTH_INPUT_Y_SCALE", "1" }, { "RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "0" },
        { "RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", "0" }, { "RESHADE_DEPTH_MULTIPLIER", "1" },
    };
    for (const auto& f : forced) {
        bool set = false;
        for (auto& d : g_definitions) if (d.first == f.first) { d.second = f.second; set = true; }
        if (!set) g_definitions.emplace_back(f.first, f.second);
    }
    const std::pair<const char*, const char*> defaults[] = {
        { "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", "2000000.0" }, { "SOURCE2_NEAR_PLANE", "12.0" },
    };
    for (const auto& f : defaults) {
        bool set = false;
        for (auto& d : g_definitions) if (d.first == f.first) set = true;
        if (!set) g_definitions.emplace_back(f.first, f.second);
    }
}

bool WriteRunConfig() {
    const auto general = IniSection(g_bin / "ReShade.ini", "GENERAL");
    std::string effects, textures;
    for (const std::string& p : SplitCommas(IniValue(general, "EffectSearchPaths", ".\\reshade-shaders\\Shaders\\**")))
        effects += (effects.empty() ? "" : ",") + Absolute(p);
    for (const std::string& p : SplitCommas(IniValue(general, "TextureSearchPaths", ".\\reshade-shaders\\Textures\\**")))
        textures += (textures.empty() ? "" : ",") + Absolute(p);
    // Our own folder of effects as well (s2fm_reshade\fx), searched after the user's: the live tests put theirs there.
    std::error_code ec;
    fs::create_directories(g_runDir / "fx", ec);
    effects += (effects.empty() ? "" : ",") + (g_runDir / "fx").u8string();
    std::string cache = IniValue(general, "IntermediateCachePath");
    if (cache.empty()) cache = (g_runDir / "cache").u8string();
    // The live tests may ask for a cache of their own (an empty one, to compile as the first time does).
    if (const char* test = getenv("RS_TEST_CACHE")) if (*test) cache = test;
    MakeDefinitions(IniValue(general, "PreprocessorDefinitions"));
    std::ofstream f(g_runIni, std::ios::binary | std::ios::trunc);
    f << "[GENERAL]\r\n"
      << "EffectSearchPaths=" << effects << "\r\n"
      << "TextureSearchPaths=" << textures << "\r\n"
      << "IntermediateCachePath=" << cache << "\r\n"
      << "NoEffectCache=" << IniValue(general, "NoEffectCache", "0") << "\r\n"
      << "NoReloadOnInit=0\r\nPerformanceMode=0\r\nSkipLoadingDisabledEffects=1\r\n"
      << "PresetPath=" << g_runPreset.u8string() << "\r\n";
    return f.good();
}

// The preset in use: a copy of 'source' (or an empty one); its stages set by hand read from [S2FM].
void UsePreset(const std::string& source) {
    std::error_code ec;
    reshade::ini_file::clear_cache(g_runPreset);
    if (!source.empty() && fs::exists(fs::u8path(source), ec)) {
        fs::copy_file(fs::u8path(source), g_runPreset, fs::copy_options::overwrite_existing, ec);
    } else {
        std::ofstream(g_runPreset, std::ios::binary | std::ios::trunc) << "Techniques=\r\n";
    }
    g_presetSource = source;
    g_hand.clear();
    for (const auto& kv : IniSection(g_runPreset, "S2FM")) {
        if (kv.first.compare(0, 6, "Stage.") != 0) continue;
        const int s = kv.second == "A" ? 0 : kv.second == "B" ? 1 : kv.second == "C" ? 2 : -1;
        if (s >= 0) g_hand[kv.first.substr(6)] = s;
    }
    g_presetEmpty = IniValue(IniSection(g_runPreset, ""), "Techniques").empty();
    g_reinit = true;
    g_stagesKnown = false;
}

fs::path StatePath() { return g_runDir / "state.ini"; }

void LoadState() {
    std::error_code ec;
    g_stateTime = fs::last_write_time(StatePath(), ec);
    g_sessions.clear();
    for (const auto& kv : IniSection(StatePath(), "SESSIONS")) g_sessions[kv.first] = kv.second;
    const auto general = IniSection(StatePath(), "GENERAL");
    g_effectsOn = IniValue(general, "EffectsOn", "1") != "0";
    reshade::log::message(reshade::log::level::info, "S2FM: %zu sessions with a preset in state.ini", g_sessions.size());
}

void SaveState() {
    std::ofstream f(StatePath(), std::ios::binary | std::ios::trunc);
    f << "[GENERAL]\r\nEnabled=" << (g_sessions.empty() ? 0 : 1) << "\r\nEffectsOn=" << (g_effectsOn ? 1 : 0)
      << "\r\n\r\n[SESSIONS]\r\n";
    for (const auto& kv : g_sessions) f << kv.first << "=" << kv.second << "\r\n";
    f.close();
    std::error_code ec;
    g_stateTime = fs::last_write_time(StatePath(), ec);
}

// The session open now, through dxgi.dll (S2FM's document); its preset comes in when it changes, unless the
// switch chose one (the tests).
void FollowSession() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_sessionLook < 1000) return;
    g_sessionLook = now;
    typedef int(__cdecl * SessionName_Fn)(char*, int);
    SessionName_Fn name = g_dxgi ? (SessionName_Fn)GetProcAddress(g_dxgi, "DxrStages_SessionName") : nullptr;
    char b[256] = "";
    if (!name || !name(b, sizeof(b))) return;
    // state.ini written by someone else since (another S2FM, a test) is read again, and its preset for the open
    // session comes in as when the session opens.
    std::error_code ec;
    const fs::file_time_type stateTime = fs::last_write_time(StatePath(), ec);
    const bool stateChanged = !ec && stateTime != g_stateTime;
    if (stateChanged) LoadState();
    if (g_session == b && !stateChanged) return;
    g_session = b;
    reshade::log::message(reshade::log::level::info, "S2FM: session \"%s\" open, its preset: \"%s\"", b,
                          g_sessions.count(g_session) ? g_sessions[g_session].c_str() : "");
    if (g_presetFromSwitch) return;
    const auto it = g_sessions.find(g_session);
    const std::string preset = it != g_sessions.end() ? it->second : std::string();
    if (preset != g_presetSource) UsePreset(preset);
}

void ReadSwitch() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_switchLook < 250) return;
    g_switchLook = now;
    std::string line;
    if (g_bin.empty()) return;
    std::ifstream f(g_bin / kSwitch);
    std::getline(f, line);
    line = Trim(line);
    if (line == g_switchLine) return;
    g_switchLine = line;
    std::string preset;
    bool presetWord = false;
    int stage = -1;
    std::istringstream words(line);
    for (std::string w; words >> w;) {
        if (w.compare(0, 7, "preset=") == 0) { preset = w.substr(7); presetWord = true; }
        else if (w == "stage=A") stage = 0;
        else if (w == "stage=B") stage = 1;
        else if (w == "stage=C") stage = 2;
    }
    // A preset named on the switch (the tests) is used; a line without one drops it again, but only when the
    // switch chose it: the panel's choice is left alone.
    if (presetWord && preset != g_presetSource) {
        UsePreset(preset);
        g_presetFromSwitch = true;
    } else if (!presetWord && g_presetFromSwitch) {
        // Back to the session's own preset, if it has one.
        const auto it = g_sessions.find(g_session);
        UsePreset(it != g_sessions.end() ? it->second : std::string());
        g_presetFromSwitch = false;
    }
    if (stage != g_testStage) { g_testStage = stage; g_stagesKnown = false; }
}

void WorkOutStages() {
    std::vector<size_t> indices;
    const std::vector<rs::technique_facts> facts = rs::core_technique_facts(&indices);
    const std::vector<int> autoStage = rs::assign_stages(facts);
    const size_t n = rs::core_technique_count();
    g_stageOf.assign(n, rs::STAGE_C);
    g_autoOf.assign(n, rs::STAGE_C);
    g_handOf.assign(n, false);
    for (size_t i = 0; i < facts.size(); ++i) {
        const size_t k = indices[i];
        if (k >= n) continue;
        g_autoOf[k] = autoStage[i];
        int stage = autoStage[i];
        const auto hand = g_hand.find(facts[i].name);
        if (hand != g_hand.end()) { stage = hand->second; g_handOf[k] = true; }
        if (g_testStage >= 0) stage = g_testStage;
        g_stageOf[k] = stage;
    }
    g_stagesKnown = true;
}

// Said once the effects are in: the log is where to see that a preset's effects were compiled and made.
void NoteLoading() {
    const bool loading = rs::core_loading();
    if (g_wasLoading && !loading && rs::core_ready()) {
        reshade::runtime* rt = rs::core_runtime();
        reshade::log::message(reshade::log::level::info, "S2FM: the effects are in: %zu techniques, %.1f s after the "
                              "runtime was made", rt ? rt->s2fm_technique_count() : (size_t)0,
                              (GetTickCount64() - g_madeAt) / 1000.0);
    }
    g_wasLoading = loading;
}

uint32_t Hash(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = 2166136261u;
    for (uint32_t v : { a, b, c }) { h ^= v; h *= 16777619u; h ^= h >> 15; }
    return h;
}

int __cdecl OnStage(const DxrStageFrame* f, void*) {
    if (!f || f->size != sizeof(DxrStageFrame) || !f->ctx) return 0;
    g_lastStageAt = GetTickCount64();
    g_lastRendering = (f->flags & DXR_STAGE_RENDERING) != 0;
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    ReadSwitch();
    FollowSession();
    // No preset, no runtime: ReShade given an empty preset loads every effect it can find, which stalls the
    // viewport for nothing.
    if (g_presetSource.empty()) {
        if (rs::core_ready()) rs::core_shutdown();
        return 0;
    }
    const bool rendering = (f->flags & DXR_STAGE_RENDERING) != 0;
    // Each stage's picture size, said when it changes (a stage of another size remakes the runtime).
    static uint32_t s_sizes[3][2] = {};
    if (f->stage >= 0 && f->stage < 3 && (s_sizes[f->stage][0] != f->width || s_sizes[f->stage][1] != f->height)) {
        s_sizes[f->stage][0] = f->width;
        s_sizes[f->stage][1] = f->height;
        reshade::log::message(reshade::log::level::info, "S2FM: stage %c is %ux%u, %u samples, encoding %d",
                              "ABC"[f->stage], f->width, f->height, f->samples, f->encoding);
    }
    // The runtime at the picture's size: made at the first call, again when the size changes for good.
    bool make = !rs::core_ready() || g_reinit;
    // Only a size that lasts makes the runtime again: a picture of the runtime's own size in between starts the
    // count over, so two views of different sizes taking turns never remake it back and forth.
    if (!make && f->width == rs::core_width() && f->height == rs::core_height()) g_otherCount = 0;
    if (!make && (f->width != rs::core_width() || f->height != rs::core_height())) {
        if (f->width == g_otherW && f->height == g_otherH) ++g_otherCount;
        else { g_otherW = f->width; g_otherH = f->height; g_otherCount = 1; }
        if (g_otherCount < 30) return 0;
        make = true;
    }
    // A preset that names no technique makes no runtime: ReShade given an empty list loads every effect it finds.
    if (make && g_presetEmpty) {
        static std::string s_said;
        if (s_said != g_presetSource) {
            s_said = g_presetSource;
            reshade::log::message(reshade::log::level::warning, "S2FM: %s names no technique; no runtime is made",
                                  g_presetSource.c_str());
        }
        if (rs::core_ready()) rs::core_shutdown();
        g_reinit = false;
        return 0;
    }
    if (make) {
        reshade::log::message(reshade::log::level::info, "S2FM: making the runtime at %ux%u (stage %c%s)", f->width,
                              f->height, "ABC"[f->stage < 0 || f->stage > 2 ? 0 : f->stage], g_reinit ? ", asked" : "");
        g_reinit = false;
        g_otherCount = 0;
        g_stagesKnown = false;
        g_madeAt = GetTickCount64();
        g_wasLoading = false;
        ID3D11Device* dev = nullptr;
        f->ctx->GetDevice(&dev);
        const bool ok = dev && rs::core_init(dev, f->ctx, f->width, f->height, g_runIni.u8string(), &g_definitions);
        if (dev) dev->Release();
        if (!ok) return 0;
        rs::core_set_effects_on(g_effectsOn);
    }
    if (rs::core_loading()) {
        // A render waits for the effects; the viewport goes on without them meanwhile.
        if (!rendering || !rs::core_wait_loaded(120000)) {
            rs::core_pump();
            NoteLoading();
            return 0;
        }
    }
    NoteLoading();
    if (!g_effectsOn) return 0;
    if (!g_stagesKnown) WorkOutStages();
    std::vector<bool> filter(g_stageOf.size(), false);
    bool any = false;
    for (size_t i = 0; i < g_stageOf.size(); ++i) if (g_stageOf[i] == f->stage) { filter[i] = true; any = true; }
    if (!any) return 0;
    rs::clock c;
    c.seconds = f->seconds;
    c.frameCount = (uint64_t)f->filmFrame * 1024u + f->sampleIndex;
    c.randomSeed = Hash(f->filmFrame, f->sampleIndex, (uint32_t)f->stage);
    return rs::core_stage(*f, c, filter) ? 1 : 0;
}

}  // namespace

// --- rs_game.h
std::recursive_mutex& rs::api_mutex() { return g_mutex; }
bool rs::game_started() { return g_started; }
fs::path rs::game_bin() { return g_bin; }
std::string rs::game_preset_source() { return g_presetSource; }
fs::path rs::game_preset_copy() { return g_runPreset; }
void rs::game_use_preset(const std::string& source) {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    UsePreset(source);
    g_presetFromSwitch = false;
    // Kept for the session open now: opening it again brings the preset back.
    if (!g_session.empty()) {
        if (source.empty()) g_sessions.erase(g_session);
        else g_sessions[g_session] = source;
        SaveState();
    }
}
void rs::game_reload() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    g_presetEmpty = IniValue(IniSection(g_runPreset, ""), "Techniques").empty();
    g_reinit = true;
    g_stagesKnown = false;
}
bool rs::game_effects_on() { return g_effectsOn; }
void rs::game_set_effects_on(bool on) {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    g_effectsOn = on;
    rs::core_set_effects_on(on);
    SaveState();
}

int rs::game_stage(size_t i, int* autoStage, bool* overridden) {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    if (!g_stagesKnown && rs::core_ready() && !rs::core_loading()) WorkOutStages();
    if (i >= g_stageOf.size()) { if (autoStage) *autoStage = STAGE_C; if (overridden) *overridden = false; return STAGE_C; }
    if (autoStage) *autoStage = g_autoOf[i];
    if (overridden) *overridden = g_handOf[i];
    return g_stageOf[i];
}

void rs::game_set_stage(const std::string& technique, int stage) {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    reshade::ini_file& ini = reshade::ini_file::load_cache(g_runPreset);
    if (stage < 0) {
        g_hand.erase(technique);
        ini.remove_key("S2FM", "Stage." + technique);
    } else {
        g_hand[technique] = stage;
        ini.set("S2FM", "Stage." + technique, std::string(stage_letter(stage)));
    }
    reshade::ini_file::flush_cache(g_runPreset);
    g_stagesKnown = false;
}

void rs::game_stages_dirty() { std::lock_guard<std::recursive_mutex> lk(g_mutex); g_stagesKnown = false; }

void rs::game_redraw() {
    typedef int(__cdecl * Redraw_Fn)();
    Redraw_Fn redraw = g_dxgi ? (Redraw_Fn)GetProcAddress(g_dxgi, "DxrStages_Redraw") : nullptr;
    if (redraw) redraw();
}

// Called by the tool on S2FM's main thread each time the tool framework runs a frame (rs_tool.cpp, RunFrame).
// ReShade makes a compiled effect on the card only as a frame begins, one an update, and the core begins one only
// at a stage, that is when S2FM draws; S2FM draws only when something changes. Chosen with the view left alone, a
// preset's effects compiled in under a second and then waited, "compiling" in the panel, until the view was
// touched. So while the effects load,
// and once after they are in (a whole picture with them), S2FM is asked to draw again when it has drawn nothing
// for a while. Never while a stage runs, and never in a render: its frames come by themselves, and a redraw
// moves the playhead.
RS_API int RsCore_Tick() {
    const ULONGLONG kQuietMs = 500;
    static ULONGLONG s_lastAsk = 0;
    static bool s_wasLoading = false, s_askAfter = false;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastAsk < kQuietMs) return 0;
    bool loading;
    {
        std::unique_lock<std::recursive_mutex> lk(g_mutex, std::try_to_lock);
        if (!lk.owns_lock() || !g_started) return 0;
        loading = rs::core_loading();
    }
    if (s_wasLoading && !loading) s_askAfter = true;
    s_wasLoading = loading;
    if (!loading && !s_askAfter) return 0;
    if (g_lastRendering || now - g_lastStageAt < kQuietMs) return 0;
    s_lastAsk = now;
    if (!loading) s_askAfter = false;
    rs::game_redraw();
    return 1;
}

RS_API int RsCore_Start(const char* gameBinWin64) {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    if (g_started) return 1;
    if (!gameBinWin64 || !*gameBinWin64) return 0;
    g_dxgi = GetModuleHandleW(L"dxgi.dll");
    auto version = g_dxgi ? (DxrStages_ApiVersion_Fn)GetProcAddress(g_dxgi, "DxrStages_ApiVersion") : nullptr;
    auto reg = g_dxgi ? (DxrStages_Register_Fn)GetProcAddress(g_dxgi, "DxrStages_Register") : nullptr;
    if (!version || !reg || version() != DXR_STAGES_API_VERSION) return 0;
    g_bin = fs::u8path(gameBinWin64);
    g_runDir = g_bin / "s2fm_reshade";
    std::error_code ec;
    fs::create_directories(g_runDir, ec);
    g_runIni = g_runDir / "run.ini";
    g_runPreset = g_runDir / "preset.ini";
    if (!WriteRunConfig()) return 0;
    rs::core_open_log();
    LoadState();
    UsePreset("");
    g_started = true;
    return reg(&OnStage, nullptr);
}
