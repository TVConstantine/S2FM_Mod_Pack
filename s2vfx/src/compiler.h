// Compiles a parsed .vfx shader into the features, vs and ps files of Half-Life: Alyx (VCS version 64). A port of
// python/s2vfxc.py: every static and dynamic combo compiled with d3dcompiler_47, each variant's reflection turned
// into the static combo's index arrays.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "library.h"
#include "vcs.h"

struct CompileError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct CompileOptions
{
    std::filesystem::path source;
    std::string name;                                           // the shader's name; the file's when empty
    std::vector<std::filesystem::path> includeDirs;
    std::vector<std::pair<std::string, std::string>> defines;   // for every variant
    int jobs = 0;                                               // HLSL compiles at once; 0 = every logical core
    // Combos built for one value only (--only, --s2fm): a variant with another value takes that value's code, so it
    // is not compiled. For a shader used in one place only (S2FM draws S_MODE_TOOLS_VIS 1, no VR multiview).
    std::vector<std::pair<std::string, int>> only;
    bool leaveOut = false;      // and the static combos with other values left out of the file (--s2fm)
    std::function<void(const std::string&)> log;
};

// {"features" | "vs" | "ps": file bytes}, and the shader's name.
std::pair<std::string, std::map<std::string, Bytes>> CompileShader(const CompileOptions& options, const Library& lib);
