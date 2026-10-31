#pragma once
// The shader table, made on the user's own machine from the game's own compiled shaders (Half-Life: Alyx's
// game\core and game\hlvr shaders_pc_dir.vpk, and loose .vcs files in game\core\shaders\vfx). The game's shaders carry
// no names (their DXBC has no reflection), so the proxy finds which game shader a compiled shader is, and at which
// slot it reads each texture, by the DXBC checksum the .vcs files give each variant. No file of Valve's ships with these packages; the table is made here instead.
//
// The file written is the one the reader (shader_table.h) loads: "DXRS" version 3.
#include <string>

struct ShaderTableReport {
    int shaders = 0, programs = 0, files = 0, entries = 0, layouts = 0, problems = 0;
    int looseFiles = 0;          // loose .vcs files in game\core\shaders\vfx, taken over the archives' own
    std::string firstProblems;   // the first few, one a line
    double seconds = 0.0;
};

// gameDir is the game folder (...\Half-Life Alyx\game). False when an archive cannot be read or the file not written.
bool BuildShaderTable(const std::wstring& gameDir, const std::wstring& outPath, ShaderTableReport* report);
