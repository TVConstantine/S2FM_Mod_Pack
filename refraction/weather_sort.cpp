// The weather volumes before the particles. The engine sorts see-through models and particles in one list, back to
// front, by the nearest point of each one's bounds: a weather volume round the camera is the nearest of all and draws
// last, over every particle. A depth sort bias on its scene object counts as distance (more is farther, drawn
// earlier); S2FM keeps none for models, so each weather model of the shot gets one here, a few times a second
// (common/sfm_doc.cpp). All of them get the same, so their own order stays the engine's: a volume round the camera
// still hazes one far off. What it costs: a particle behind a volume, seen through it, now draws over it.
//
// A weather model is one whose material override hands it a material of the s2vfx_weather shader, read from the
// compiled material (.vmat_c, the shader's name is in it as text) in the addon, hlvr or core, once a material.
//
// Test word (s2fm_refract_debug.txt, read by refract.cpp): nosortbias   the engine's own order (biases given back).
#include "../core/core.h"
#include "../common/sfm_doc.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

void Log(const char* fmt, ...);

namespace {

// Farther than anything in a map, still exact enough as a float for the distances added to it.
const float kBias = 1.0e7f;
const unsigned long long kEveryMs = 250, kRetryMissingMs = 10000;

std::atomic<bool> g_off{false};

struct Known {
    bool weather = false;
    bool found = false;
    unsigned long long at = 0;
};
std::unordered_map<std::string, Known> g_known;
std::wstring g_roots[3];   // the addon's game folder, hlvr's, core's; each with a trailing slash
bool g_rootsMade = false;

void MakeRoots() {
    g_rootsMade = true;
    wchar_t self[MAX_PATH] = {};
    HMODULE me = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&MakeRoots, &me);
    GetModuleFileNameW(me, self, MAX_PATH);
    // game\bin\win64\dxgi.dll -> game\ .
    std::wstring game = self;
    for (int up = 0; up < 3; ++up) {
        const size_t slash = game.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        game.resize(slash);
    }
    game += L'\\';
    // The addon from the command line (-addon name).
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; argv && i + 1 < argc; ++i)
        if (!_wcsicmp(argv[i], L"-addon")) g_roots[0] = game + L"hlvr_addons\\" + argv[i + 1] + L"\\";
    if (argv) LocalFree(argv);
    g_roots[1] = game + L"hlvr\\";
    g_roots[2] = game + L"core\\";
}

// The material's compiled file read, and whether the s2vfx_weather shader is named in it; found false when no file is.
Known Look(const std::string& rel) {
    Known k;
    if (!g_rootsMade) MakeRoots();
    std::wstring wrel(rel.begin(), rel.end());
    for (const std::wstring& root : g_roots) {
        if (root.empty()) continue;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (root + wrel).c_str(), L"rb") || !f) continue;
        static char buf[1 << 16];
        const size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        k.found = true;
        static const char kName[] = "s2vfx_weather";
        for (size_t i = 0; i + sizeof(kName) - 1 <= n && !k.weather; ++i)
            k.weather = !memcmp(buf + i, kName, sizeof(kName) - 1);
        break;
    }
    return k;
}

// For the document walk: a material path as S2FM's override holds it ("materials/x/y.vmat", a name without its
// extension, a trailing space) to its compiled file, looked at once (a missing one again after a while).
bool IsWeather(const char* material) {
    std::string rel;
    for (const char* p = material; *p; ++p) rel += *p == '/' ? '\\' : (char)tolower((unsigned char)*p);
    while (!rel.empty() && (rel.back() == ' ' || rel.back() == '\t')) rel.pop_back();
    if (rel.empty()) return false;
    if (rel.size() < 5 || rel.compare(rel.size() - 5, 5, ".vmat")) rel += ".vmat";
    rel += "_c";
    const unsigned long long now = GetTickCount64();
    auto it = g_known.find(rel);
    if (it != g_known.end() && (it->second.found || now - it->second.at < kRetryMissingMs)) return it->second.weather;
    Known k = Look(rel);
    k.at = now;
    // Only a weather material is told (the game's own are in its packages, never found here).
    if (k.weather != (it != g_known.end() && it->second.weather))
        Log("[SORT] %s: %s\n", rel.c_str(), k.weather ? "a weather material" : "no longer a weather material");
    g_known[rel] = k;
    return k.weather;
}

bool NoneIsWeather(const char*) { return false; }

void Present(IDXGISwapChain*) {
    static unsigned long long last = 0;
    const unsigned long long now = GetTickCount64();
    if (now - last < kEveryMs) return;
    last = now;
    SfmSortedModel models[64];
    const int n = SfmWeatherSortBias(g_off.load(std::memory_order_relaxed) ? NoneIsWeather : IsWeather, kBias, models, 64);
    static int saidCount = -2;
    if (n != saidCount) {
        saidCount = n;
        if (n < 0) Log("[SORT] the shot's models could not be read\n");
        else Log("[SORT] %d weather model(s) in the shot at the playhead\n", n);
    }
    for (int i = 0; i < n; ++i) {
        const SfmSortedModel& m = models[i];
        if (m.written) Log("[SORT] %s: depth sort bias %g\n", m.name, m.bias);
        else if (m.noObject) {
            static std::string said;
            if (said.find(m.name) == std::string::npos) {
                said += std::string(m.name) + "|";
                Log("[SORT] %s: no scene object to give the bias to (yet)\n", m.name);
            }
        }
    }
}

const CoreFeature kFeature = {"weather before the particles", nullptr, nullptr, nullptr, Present, nullptr, nullptr};

}  // namespace

void WeatherSortOff(bool off) { g_off.store(off, std::memory_order_relaxed); }

void WeatherSortRegister() { CoreRegister(&kFeature); }
