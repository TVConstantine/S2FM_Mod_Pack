// Whose window a frame is for.
//
// The tools draw other views besides S2FM's: the Asset Browser's and the Particle Editor's previews. Those draw into
// the engine window's 1920x1080 targets and present into their own windows, so neither the target nor the order of
// presents says whose a frame is. The view's own size does: PerView's g_vViewportSize is the size of the window the
// frame is drawn for. A frame whose size is that of another tool's window, and of no window of S2FM's, is that tool's.
// S2FM's are its viewports (the window QViewportArea, under S2FM's main window) and the engine's own window, which a
// movie render presents into; a size no window has (Image to Clipboard at another resolution) is S2FM's too.
#include "core.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {
struct ViewWindow {
    IDXGISwapChain* sc = nullptr;
    HWND hwnd = nullptr;
    bool s2fm = false;
    UINT w = 0, h = 0;
    ULONGLONG at = 0;   // its last present
};
ViewWindow g_windows[32];
int g_windowCount = 0;
std::mutex g_windowMutex;
std::atomic<int> g_foreignBinds{0}, g_oursBinds{0};

bool IsS2fmWindow(HWND hwnd, char* title, char* rootTitle, size_t size) {
    title[0] = rootTitle[0] = 0;
    wchar_t wt[256] = {};
    // InternalGetWindowText: never a message to the window's thread from Present.
    if (hwnd && InternalGetWindowText(hwnd, wt, 256) > 0)
        WideCharToMultiByte(CP_UTF8, 0, wt, -1, title, (int)size, nullptr, nullptr);
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (root && InternalGetWindowText(root, wt, 256) > 0)
        WideCharToMultiByte(CP_UTF8, 0, wt, -1, rootTitle, (int)size, nullptr, nullptr);
    if (strcmp(title, "QViewportArea") == 0) return true;
    if (strstr(rootTitle, "Source Filmmaker")) return true;
    return strcmp(rootTitle, "Half-Life: Alyx") == 0;
}
}  // namespace

// Every present: the window's size and whose it is, looked at again when the swapchain draws into another window.
// And every ten seconds, how many target binds were another tool's.
void CoreExportWatch();

void CoreViewsPresent(IDXGISwapChain* sc) {
    if (!sc) return;
    CoreExportWatch();
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d))) return;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(g_windowMutex);
        ViewWindow* v = nullptr;
        for (int i = 0; i < g_windowCount; ++i) if (g_windows[i].sc == sc) { v = &g_windows[i]; break; }
        if (!v) {
            int k = g_windowCount < 32 ? g_windowCount++ : -1;
            if (k < 0) {   // full: the one that presented longest ago makes room
                k = 0;
                for (int i = 1; i < 32; ++i) if (g_windows[i].at < g_windows[k].at) k = i;
            }
            v = &g_windows[k];
            *v = ViewWindow();
            v->sc = sc;
        }
        if (v->hwnd != d.OutputWindow || !v->at) {
            char title[256], rootTitle[256];
            v->hwnd = d.OutputWindow;
            v->s2fm = IsS2fmWindow(d.OutputWindow, title, rootTitle, sizeof(title));
            Log("[VIEWS] swapchain %p presents into '%s' under '%s': %s\n", (void*)sc, title, rootTitle,
                v->s2fm ? "S2FM's" : "another tool's, its frames are left alone");
        }
        v->w = d.BufferDesc.Width;
        v->h = d.BufferDesc.Height;
        v->at = now;
    }
    static ULONGLONG last = 0;
    if (now - last >= 10000) {
        last = now;
        const int foreign = g_foreignBinds.exchange(0), ours = g_oursBinds.exchange(0);
        if (foreign)
            Log("[VIEWS] in 10 s: %d target binds were for another tool's window and left alone, %d for S2FM's\n",
                foreign, ours);
    }
}

bool CoreSwapChainIsS2fm(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lk(g_windowMutex);
    for (int i = 0; i < g_windowCount; ++i) if (g_windows[i].sc == sc) return g_windows[i].s2fm;
    return true;
}

// Decided at the first draw after a bind of targets that has a PerView bound; until one has, the context's last
// answer stands.
bool CoreFrameForeign(ContextState* st) {
    if (!st) return false;
    if (st->foreignGen == st->rtBindGen) return st->foreign;
    float pv[204];
    if (!CorePerView(st, pv)) return st->foreign;
    const float vw = pv[PV_VIEWPORT_SIZE], vh = pv[PV_VIEWPORT_SIZE + 1];
    if (!std::isfinite(vw) || !std::isfinite(vh) || vw < 1.0f || vh < 1.0f) return st->foreign;
    st->foreignGen = st->rtBindGen;
    const UINT w = (UINT)(vw + 0.5f), h = (UINT)(vh + 0.5f);
    const ULONGLONG now = GetTickCount64();
    bool s2fm = false, other = false;
    {
        std::lock_guard<std::mutex> lk(g_windowMutex);
        for (int i = 0; i < g_windowCount; ++i) {
            const ViewWindow& v = g_windows[i];
            if (v.w != w || v.h != h || now - v.at > 5000) continue;
            (v.s2fm ? s2fm : other) = true;
        }
    }
    // Everything while a movie is rendered is S2FM's.
    st->foreign = other && !s2fm && !CoreExportActive();
    (st->foreign ? g_foreignBinds : g_oursBinds).fetch_add(1, std::memory_order_relaxed);
    return st->foreign;
}

// --- Whether a movie is being rendered to disk: S2FM's "Movie Layoff Progress" dialog is up (copied from the main
// project). The dialog is a Qt window with no swapchain of its own, so a thread looks for it by its title twice a
// second, with InternalGetWindowText, which sends the window nothing.
namespace {
std::atomic<ULONGLONG> g_exportTick{0};

DWORD WINAPI ExportWatcher(LPVOID) {
    bool was = false;
    for (;;) {
        bool found = false;
        EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
            wchar_t t[128] = {};
            if (InternalGetWindowText(w, t, 128) <= 0) return TRUE;
            if (wcsstr(t, L"Layoff Progress")) {
                *reinterpret_cast<bool*>(lp) = true;
                return FALSE;
            }
            return TRUE;
        }, (LPARAM)&found);
        if (found) g_exportTick.store(GetTickCount64(), std::memory_order_relaxed);
        if (found != was) {
            Log("[EXPORT] the Movie Layoff Progress window %s\n", found ? "is up: a movie is being rendered" : "has closed");
            was = found;
        }
        Sleep(500);
    }
}
}  // namespace

void CoreExportWatch() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    HANDLE h = CreateThread(nullptr, 0, ExportWatcher, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

bool CoreExportActive() {
    const ULONGLONG t = g_exportTick.load(std::memory_order_relaxed);
    return t != 0 && GetTickCount64() - t < 1500;
}
