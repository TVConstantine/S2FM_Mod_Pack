// s2vfxc: compiles a .vfx shader source into Half-Life: Alyx's .vcs files (version 64).
//
//     s2vfxc <shader.vfx> [--out folder] [--install] [--name shader_name] [-D NAME[=VALUE]]...
//            [--game <game folder>] [--include <folder>] [--quiet] [--only NAME=VALUE]... [--s2fm]
//            [--pack-level 0..9]
//
// --only NAME=VALUE builds combo NAME for that value only: every variant with another value is given that value's
// code instead of its own, which saves its compile. --s2fm is --only S_MODE_TOOLS_VIS=1 --only
// D_MULTIVIEW_INSTANCING=0, the variants S2FM draws with, and gives the game mode's pixel shader static combos no
// code (half the file): a shader built so is for S2FM only, it does not draw in the game or in VR. Use the full build for
// a shader meant for those. --pack-level is the blocks' LZMA level (9, as the game's files, unless given).
//
// --install writes the files to both places the engine reads a shader from (game\shaders\vfx and
// game\core\shaders\vfx), and the shader's empty .ini beside the second. The game folder is Half-Life: Alyx's "game" folder (Steam's default place unless given);
// the compiler reads the engine's buffer and resource descriptions from its shader archives. Include files are
// looked for in --include, then in an "include" folder beside the program or above it.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "compiler.h"
#include "hlsl.h"

namespace fs = std::filesystem;

namespace
{
const char* USAGE =
    "usage: s2vfxc <shader.vfx> [--out folder] [--install] [--name shader_name] [-D NAME[=VALUE]]...\n"
    "              [--game <game folder>] [--include <folder>] [--quiet] [--only NAME=VALUE]... [--s2fm]\n"
    "              [--pack-level 0..9]\n";

fs::path ProgramDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(std::wstring(buf, n)).parent_path();
}

// An "include" folder holding s2vfx/, beside the program or up to three folders above it (a build tree).
fs::path FindIncludeDir()
{
    fs::path dir = ProgramDir();
    for (int i = 0; i < 4 && !dir.empty(); i++)
    {
        if (fs::exists(dir / "include" / "s2vfx"))
            return dir / "include";
        if (dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return {};
}

void WriteFile(const fs::path& path, const Bytes& data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out)
        throw CompileError("cannot write " + path.string());
}

int Run(int argc, char** argv)
{
    CompileOptions options;
    fs::path out, include;
    fs::path game = "C:/Program Files (x86)/Steam/steamapps/common/Half-Life Alyx/game";
    bool install = false, quiet = false;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                throw CompileError(a + " needs a value");
            return argv[++i];
        };
        if (a == "--out")
            out = next();
        else if (a == "--install")
            install = true;
        else if (a == "--name")
            options.name = next();
        else if (a == "--game")
            game = next();
        else if (a == "--include")
            include = next();
        else if (a == "--quiet")
            quiet = true;
        else if (a == "--jobs")
            options.jobs = std::stoi(next());
        else if (a == "--only")
        {
            std::string d = next();
            size_t eq = d.find('=');
            if (eq == std::string::npos)
                throw CompileError("--only needs NAME=VALUE");
            options.only.emplace_back(d.substr(0, eq), std::stoi(d.substr(eq + 1)));
        }
        else if (a == "--pack-level")
            vcs::SetLzmaLevel(std::stoi(next()));
        else if (a == "--s2fm")
        {
            options.only.emplace_back("S_MODE_TOOLS_VIS", 1);
            options.only.emplace_back("D_MULTIVIEW_INSTANCING", 0);
            options.leaveOut = true;
        }
        else if (a == "-D" || (a.size() > 2 && a.compare(0, 2, "-D") == 0))
        {
            std::string d = a == "-D" ? next() : a.substr(2);
            size_t eq = d.find('=');
            options.defines.emplace_back(d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1));
        }
        else if (a == "-h" || a == "--help")
        {
            std::fputs(USAGE, stdout);
            return 0;
        }
        else if (!a.empty() && a[0] == '-')
            throw CompileError("unknown option " + a);
        else if (options.source.empty())
            options.source = a;
        else
            throw CompileError("more than one source: " + a);
    }
    if (options.source.empty())
    {
        std::fputs(USAGE, stderr);
        return 2;
    }
    if (include.empty())
        include = FindIncludeDir();
    if (!include.empty())
        options.includeDirs.push_back(include);
    if (!quiet)
        options.log = [](const std::string& s) { std::printf("%s\n", s.c_str()); };

    hlsl::UseGameCompiler(game);
    Library lib = Library::Load(game);
    auto [name, files] = CompileShader(options, lib);

    std::vector<fs::path> dirs;
    if (!out.empty())
        dirs.push_back(out);
    if (install)
    {
        dirs.push_back(game / "shaders" / "vfx");
        dirs.push_back(game / "core" / "shaders" / "vfx");
    }
    for (const auto& d : dirs)
    {
        fs::create_directories(d);
        for (const auto& [program, data] : files)
            WriteFile(d / (name + "_pc_50_" + program + ".vcs"), data);
    }
    // The engine finds which mod a shader belongs to by an empty <name>.ini beside its files (vfxcompile writes one;
    // the game's shader archives hold one per shader). Without it a material records its shader with no mod.
    if (install)
        WriteFile(game / "core" / "shaders" / "vfx" / (name + ".ini"), Bytes{});
    std::printf("%s: features %zu bytes, vs %zu bytes, ps %zu bytes\n", name.c_str(), files["features"].size(),
                files["vs"].size(), files["ps"].size());
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
