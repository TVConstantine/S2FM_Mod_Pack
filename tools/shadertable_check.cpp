// Builds the shader table from the game's own files and compares it, byte for byte, with one made elsewhere, when one
// is given.
//
//     shadertable_check <game folder> <out.bin> [reference.bin]
#include "../common/shader_table_build.h"
#include "../common/shader_table.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void Log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
}

// The reader on the table just written: a few entries looked up by their checksum, as a shader's DXBC would be.
static bool ReaderCheck(const wchar_t* path, const std::vector<unsigned char>& table) {
    ShaderDbSetPath(path);
    if (!ShaderDbReady()) { printf("the reader did not load it\n"); return false; }
    const unsigned* h = (const unsigned*)table.data();
    const size_t entries = h[6], at = table.size() - entries * 28;
    int named = 0;
    for (size_t i = 0; i < entries; i += entries / 7 + 1) {
        unsigned char dxbc[20] = { 'D', 'X', 'B', 'C' };
        memcpy(dxbc + 4, table.data() + at + i * 28, 16);
        uint16_t flags = 0;
        int combo = -1;
        const int layout = ShaderDbLookup(dxbc, sizeof(dxbc), &flags, &combo);
        printf("  entry %zu: layout %d, %s, %d fields, combo %d\n", i, layout, ShaderDbName(layout),
               ShaderDbFieldCount(layout), combo);
        named += layout >= 0 && ShaderDbName(layout)[0];
    }
    return named > 0;
}

static bool ReadAll(const wchar_t* path, std::vector<unsigned char>& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: shadertable_check <game folder> <out.bin> [reference.bin]\n");
        return 2;
    }
    ShaderTableReport rep;
    const bool ok = BuildShaderTable(argv[1], argv[2], &rep);
    printf("%s: %d shaders, %d programs, %d compiled files, %d distinct bytecodes, %d layouts, %d problems, %.1f s\n",
           ok ? "written" : "FAILED", rep.shaders, rep.programs, rep.files, rep.entries, rep.layouts, rep.problems,
           rep.seconds);
    if (!rep.firstProblems.empty()) printf("%s", rep.firstProblems.c_str());
    if (!ok) return 1;
    std::vector<unsigned char> a, b;
    if (!ReadAll(argv[2], a)) {
        printf("could not read the table back\n");
        return 1;
    }
    if (!ReaderCheck(argv[2], a)) return 1;
    if (argc < 4) return 0;
    if (!ReadAll(argv[3], b)) {
        printf("could not read the reference\n");
        return 1;
    }
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    size_t at = 0;
    while (at < n && a[at] == b[at]) ++at;
    if (at == n && a.size() == b.size()) {
        printf("the same as the reference, %zu bytes\n", a.size());
        return 0;
    }
    printf("DIFFERENT from the reference: %zu bytes against %zu, first difference at byte %zu\n", a.size(), b.size(), at);
    const unsigned* ha = (const unsigned*)a.data();
    const unsigned* hb = (const unsigned*)b.data();
    if (a.size() >= 28 && b.size() >= 28)
        printf("header: strings %u/%u layouts %u/%u axis sets %u/%u combos %u/%u entries %u/%u\n", ha[2], hb[2], ha[3],
               hb[3], ha[4], hb[4], ha[5], hb[5], ha[6], hb[6]);
    return 1;
}
