// What the engine supplies to a model shader, read from the game's own shader files (descriptions only, no code):
// the variable of every engine resource and value a shader can use, the control byte the engine's index arrays carry
// with each constant buffer (per stage), the constant buffer descriptions, and the variable hash every file of the
// game shares. A port of python/library.py.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>

#include "vcs.h"

struct Library
{
    Bytes variableHash;
    int32_t variableSourceMax = 0;
    std::map<std::string, vcs::Variable> variables;
    std::map<std::string, vcs::CBuffer> cbuffers;
    std::map<std::pair<std::string, std::string>, int> control;    // (program, buffer name): byte

    static Library Load(const std::filesystem::path& game);
};
