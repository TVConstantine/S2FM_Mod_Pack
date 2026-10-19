// The shader table beside the DLL (s2fm_shader_table.bin): made from the game's own shaders the first time the core
// runs, and again when the game's shader archives are newer than it (a Steam update), on a thread of its own, about
// half a minute. Until it is loaded shaders are not named; each is looked up again when next bound
// (core_hooks.cpp, LayoutOf).
#include "core.h"
#include "../common/shader_table.h"
#include "../common/shader_table_build.h"

#include <atomic>
#include <thread>

namespace {
std::atomic<uint32_t> g_gen{0};
std::atomic<bool> g_started{false};

ULONGLONG WriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

void Run() {
    const std::wstring table = CorePath(L"s2fm_shader_table.bin");
    const std::wstring game = CoreGameDir();
    const ULONGLONG made = WriteTime(table);
    const ULONGLONG shaders = (std::max)(WriteTime(game + L"\\core\\shaders_pc_dir.vpk"),
                                         WriteTime(game + L"\\hlvr\\shaders_pc_dir.vpk"));
    if (!made || made < shaders) {
        Log("[TABLE] %s: made from the game's shaders now\n", made ? "older than the game's shaders" : "not there yet");
        const std::wstring tmp = table + L".part";
        ShaderTableReport rep;
        if (BuildShaderTable(game, tmp, &rep) && MoveFileExW(tmp.c_str(), table.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            Log("[TABLE] made: %d shaders, %d compiled files, %d layouts, %d problems, %.1f s\n", rep.shaders, rep.files,
                rep.layouts, rep.problems, rep.seconds);
        } else {
            Log("[TABLE] could not be made (%d problems)%s%s\n", rep.problems, rep.firstProblems.empty() ? "" : ":\n",
                rep.firstProblems.c_str());
            DeleteFileW(tmp.c_str());
        }
    }
    ShaderDbSetPath(table.c_str());
    for (int tries = 0; tries < 30; ++tries) {
        if (ShaderDbReady()) {
            g_gen.store(1, std::memory_order_release);
            return;
        }
        Sleep(1000);
    }
    Log("[TABLE] not loaded: shaders stay unnamed this session\n");
}
}  // namespace

void CoreTableStart() {
    if (g_started.exchange(true)) return;
    std::thread(Run).detach();
}

uint32_t CoreTableGen() { return g_gen.load(std::memory_order_acquire); }

bool CoreTableReady() { return CoreTableGen() != 0; }
