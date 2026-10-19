// Global new and delete through tier0, the way the game's Qt allocates: Qt deletes the children and the posted events this module makes,
// and this module deletes what Qt makes, so both must come from the same allocator. Without tier0 (a
// test program) the C runtime's heap serves both halves.
#include <windows.h>
#include <new>
#include <stdlib.h>

namespace {
typedef void* (*AllocFn)(size_t);
typedef void (*FreeFn)(void*);

struct Heap
{
    AllocFn alloc;
    FreeFn release;
};

Heap FindHeap()
{
    Heap h = { &malloc, &free };
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (tier0) {
        AllocFn a = (AllocFn)GetProcAddress(tier0, "MemAlloc_AllocFunc");
        FreeFn f = (FreeFn)GetProcAddress(tier0, "MemAlloc_FreeFunc");
        if (a && f) { h.alloc = a; h.release = f; }
    }
    return h;
}

// Decided once, by the first allocation, and never changed: a block must go back where it came from.
const Heap& TheHeap()
{
    static const Heap h = FindHeap();
    return h;
}

void* Allocate(size_t n)
{
    void* p = TheHeap().alloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void Release(void* p)
{
    if (p) TheHeap().release(p);
}
}

void* operator new(size_t n) { return Allocate(n); }
void* operator new[](size_t n) { return Allocate(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept
{
    try { return Allocate(n); } catch (...) { return nullptr; }
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept
{
    try { return Allocate(n); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { Release(p); }
void operator delete[](void* p) noexcept { Release(p); }
void operator delete(void* p, size_t) noexcept { Release(p); }
void operator delete[](void* p, size_t) noexcept { Release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Release(p); }
