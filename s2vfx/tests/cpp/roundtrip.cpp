// Every .vcs of the game read and written back by the C++ reader and writer: the bytes must be the same, each static
// combo block too; every code's stored hash must be its MD5. Files under 2 MB also have every block packed again
// (raw, stored, LZMA) and read back: the data must be the same. The C++ twin of tests/roundtrip.py.
//
//     vcs_roundtrip [name filter] [--game <game folder>]
#include <chrono>
#include <cstdio>
#include <string>

#include "vcs.h"
#include "vpk.h"

int Run(int argc, char** argv)
{
    std::string want;
    std::filesystem::path game = "C:/Program Files (x86)/Steam/steamapps/common/Half-Life Alyx/game";
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc)
            game = argv[++i];
        else
            want = a;
    }
    auto t0 = std::chrono::steady_clock::now();
    auto files = vpk::ShaderFiles(game);
    int count = 0, bad = 0, hashesOk = 0, hashesAll = 0;
    for (const auto& [name, entry] : files)
    {
        if (name.find(want) == std::string::npos || name.find("_pc_") == std::string::npos)
            continue;
        Bytes data = vpk::Read(entry);
        std::string program = name.substr(name.rfind('_') + 1);
        program = program.substr(0, program.size() - 4);
        if (data.size() < 8 || data[4] != 64)
        {
            std::printf("skip (version %d) %s\n", data.size() > 4 ? data[4] : -1, name.c_str());
            continue;
        }
        count++;
        bool same = true;
        try
        {
            vcs::File f = vcs::Read(data, program);
            same = vcs::Write(f) == data;
            for (const auto& b : f.blocks)
            {
                auto sc = vcs::ReadStaticCombo(b.data, program);
                if (vcs::WriteStaticCombo(sc, program) != b.data)
                    same = false;
                for (const auto& fl : sc.files)
                {
                    hashesAll++;
                    hashesOk += vcs::Md5(fl.code()) == fl.hash;
                }
            }
            if (data.size() < (2u << 20))
                for (auto how : {vcs::Pack::Raw, vcs::Pack::Stored, vcs::Pack::Lzma})
                {
                    vcs::File g = vcs::Read(vcs::Write(f, how), program);
                    for (size_t i = 0; i < f.blocks.size(); i++)
                        if (g.blocks[i].data != f.blocks[i].data)
                            same = false;
                }
        }
        catch (const std::exception& e)
        {
            same = false;
            std::printf("  error %s\n", e.what());
        }
        if (!same)
        {
            bad++;
            std::printf("FAIL %s\n", name.c_str());
        }
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d files, %d failed, %.0f s\n", count, bad, s);
    std::printf("bytecode hash = MD5 of the code: %d of %d\n", hashesOk, hashesAll);
    return bad ? 1 : 0;
}

int main(int argc, char** argv)
{
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::printf("error: %s\n", e.what());
        return 2;
    }
}
