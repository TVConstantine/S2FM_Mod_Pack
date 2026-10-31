#include "vpk.h"

#include <cstdio>
#include <fstream>

namespace vpk
{

Bytes ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw FormatError("cannot open " + path.string());
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::map<std::string, Entry> Entries(const std::filesystem::path& dirVpk, const std::string& suffix)
{
    Bytes b = ReadFile(dirVpk);
    Reader r(b);
    if (r.take<uint32_t>() != 0x55AA1234)
        throw FormatError("not a VPK directory: " + dirVpk.string());
    uint32_t version = r.take<uint32_t>();
    uint32_t tree = r.take<uint32_t>();
    uint32_t header = version == 1 ? 12 : 28;
    Reader t(b, header);
    std::map<std::string, Entry> out;
    for (;;)
    {
        std::string ext = t.cstr();
        if (ext.empty())
            break;
        for (;;)
        {
            std::string dir = t.cstr();
            if (dir.empty())
                break;
            for (;;)
            {
                std::string name = t.cstr();
                if (name.empty())
                    break;
                Entry e;
                e.dir = dirVpk;
                t.take<uint32_t>();                 // crc
                uint16_t preload = t.take<uint16_t>();
                e.archive = t.take<uint16_t>();
                e.offset = t.take<uint32_t>();
                e.length = t.take<uint32_t>();
                t.take<uint16_t>();                 // terminator
                e.preload = t.raw(preload);
                e.dirEnd = header + tree;
                std::string file = name + "." + ext;
                if (file.size() >= suffix.size() && file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0)
                    out[file] = std::move(e);
            }
        }
    }
    return out;
}

Bytes Read(const Entry& e)
{
    std::filesystem::path path = e.dir;
    uint64_t offset = e.offset;
    if (e.archive == 0x7FFF)
        offset += e.dirEnd;
    else
    {
        std::string name = e.dir.filename().string();
        char number[16];
        std::snprintf(number, sizeof(number), "_%03u.vpk", e.archive);
        name.replace(name.find("_dir.vpk"), 8, number);
        path = e.dir.parent_path() / name;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw FormatError("cannot open " + path.string());
    Bytes out = e.preload;
    size_t start = out.size();
    out.resize(start + e.length);
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(out.data() + start), e.length);
    if (static_cast<uint32_t>(in.gcount()) != e.length)
        throw FormatError("short read in " + path.string());
    return out;
}

std::map<std::string, Entry> ShaderFiles(const std::filesystem::path& game)
{
    std::map<std::string, Entry> out;
    for (const char* mod : {"core", "hlvr"})
    {
        auto dir = game / mod / "shaders_pc_dir.vpk";
        if (!std::filesystem::exists(dir))
            continue;
        for (auto& [name, e] : Entries(dir, ".vcs"))
            out[name] = e;      // a later archive wins, as in python/vpk.py
    }
    return out;
}

} // namespace vpk
