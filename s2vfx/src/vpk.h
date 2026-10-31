// Reads files out of a game's VPK archives (version 1 and 2 directories).
#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "bytes.h"

namespace vpk
{

struct Entry
{
    std::filesystem::path dir;      // the _dir.vpk
    uint16_t archive = 0;           // 0x7FFF: in the directory file itself
    uint32_t offset = 0, length = 0;
    Bytes preload;
    uint32_t dirEnd = 0;            // where the directory's data starts
};

// {file name: entry} of every entry ending in suffix.
std::map<std::string, Entry> Entries(const std::filesystem::path& dirVpk, const std::string& suffix);
Bytes Read(const Entry& e);

// {file name: entry} of every DirectX 11 .vcs in a game's shader archives (core and hlvr).
std::map<std::string, Entry> ShaderFiles(const std::filesystem::path& game);

Bytes ReadFile(const std::filesystem::path& path);

} // namespace vpk
