#include "library.h"

#include <set>
#include <vector>

#include "vpk.h"

Library Library::Load(const std::filesystem::path& game)
{
    static const char* BASES[] = {"vr_black_unlit", "vr_simple", "generic", "spritecard", "vr_projected_decals"};
    auto files = vpk::ShaderFiles(game);
    if (files.empty())
        throw FormatError("no shader archives under " + game.string());
    Library lib;
    std::set<std::pair<Bytes, int32_t>> hashes;
    // Counts of each control byte, in the order first seen (the most common wins, the first on a tie).
    std::map<std::pair<std::string, std::string>, std::vector<std::pair<int, int>>> counts;
    for (const char* base : BASES)
        for (const char* program : {"features", "vs", "ps"})
        {
            std::string name = std::string(base) + "_pc_50_" + program + ".vcs";
            auto it = files.find(name);
            if (it == files.end() || (std::string(program) != "features" && it->second.length > 3000000))
                continue;
            vcs::File f = vcs::Read(vpk::Read(it->second), program);
            hashes.insert({f.variableHash, f.variableSourceMax});
            for (const auto& v : f.variables)
            {
                std::string n = vcs::Text(v.name);
                // Engine resources and values: what an attribute supplies (source 1) and the constant buffers.
                if ((v.source == 1 || v.reg == 8) && !lib.variables.count(n))
                    lib.variables[n] = v;
            }
            for (const auto& cb : f.cbuffers)
                lib.cbuffers.emplace(vcs::Text(cb.name), cb);
            if (std::string(program) == "features")
                continue;
            // Every block: the tools blocks (S_MODE_TOOLS_VIS) are the only ones binding S2FM's light buffers.
            for (const auto& b : f.blocks)
            {
                auto sc = vcs::ReadStaticCombo(b.data, program);
                std::vector<const vcs::IndexArray*> arrays{&sc.all};
                for (const auto& a : sc.dynamic)
                    arrays.push_back(&a);
                for (const auto* a : arrays)
                    for (const auto& field : a->fields)
                    {
                        const auto& v = f.variables[field.var & 0xFFF];
                        if (v.reg != 8)
                            continue;
                        auto& c = counts[{program, vcs::Text(v.name)}];
                        int byte = field.slot >> 8;
                        bool found = false;
                        for (auto& [k, n] : c)
                            if (k == byte)
                            {
                                n++;
                                found = true;
                            }
                        if (!found)
                            c.emplace_back(byte, 1);
                    }
            }
        }
    if (hashes.size() != 1)
        throw FormatError("the game's files disagree on the variable hash");
    lib.variableHash = hashes.begin()->first;
    lib.variableSourceMax = hashes.begin()->second;
    for (const auto& [key, c] : counts)
    {
        auto best = c.front();
        for (const auto& e : c)
            if (e.second > best.second)
                best = e;
        lib.control[key] = best.first;
    }
    return lib;
}
