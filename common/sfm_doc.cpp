// S2FM's open document, read from inside the process: the playhead, the session's movie name, the frame rate, the
// SSAO settings of the camera at the playhead, and a redraw. Only the 6 May 2022 sfm.dll is read: the offsets below are that build's,
// and the build is checked by its timestamp before anything is read. Every read is inside one __try.
#include "sfm_doc.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void Log(const char* fmt, ...);

namespace {

const uint32_t kStamp2022 = 0x6275AD57;
// g_pDataModel, which holds &g_DataModel (rva 0x8BBB30) from the moment sfm.dll is loaded.
const uint32_t kDataModelPtr = 0x884288;
const uint32_t kDataModelVtable = 0x6B8558;

// CDataModel: the handle table, 16 bytes an entry, the serial in the low 31 bits of the first
// dword (bit 31 set when the slot is free) and the element at +8. See its GetElement, vtable+0xA8.
const size_t kHandleCount = 0x90, kHandleTable = 0x98, kHandleEntry = 16;
const int kVfGetElement = 0xA8 / 8, kVfTypeName = 0xC8 / 8;
// CDmElement
const size_t kElemId = 0x08, kElemHandle = 0x28, kElemFirstAttr = 0x48, kElemType = 0x50;
// CDmAttribute: a list through +0, the value behind the pointer at +8, the type in the low six
// bits of +0x1C, the name at +0x20. An array's value is a CUtlVector: the count at +0, the items
// at +8.
const size_t kAttrNext = 0x00, kAttrData = 0x08, kAttrFlags = 0x1C, kAttrName = 0x20;
const int kTypeMask = 0x3F, kAtElement = 1, kAtString = 5, kAtFirstArray = 33, kAtElementArray = 33;

typedef void* (__fastcall* GetElementFn)(void* dm, uint32_t handle);
typedef const char* (__fastcall* TypeNameFn)(void* dm, int type);

struct Doc
{
    uint8_t* base;
    void* dm;
    GetElementFn getElement;
    TypeNameFn typeName;
    FILE* out;
    int elementsWritten;
};

template <class T> T At(const void* p, size_t off) { return *(const T*)((const uint8_t*)p + off); }

const char* TypeOf(const void* e) { const char* t = At<const char*>(e, kElemType); return t ? t : "?"; }
int AttrType(const void* a) { return At<uint32_t>(a, kAttrFlags) & kTypeMask; }
const char* AttrName(const void* a) { const char* n = At<const char*>(a, kAttrName); return n ? n : "?"; }

const void* FindAttr(const void* e, const char* name)
{
    for (const void* a = At<const void*>(e, kElemFirstAttr); a; a = At<const void*>(a, kAttrNext))
        if (strcmp(AttrName(a), name) == 0) return a;
    return nullptr;
}

const char* StringOf(const void* e, const char* name)
{
    const void* a = FindAttr(e, name);
    if (!a || AttrType(a) != kAtString) return "";
    const void* data = At<const void*>(a, kAttrData);
    const char* s = data ? At<const char*>(data, 0) : nullptr;
    return s ? s : "";
}

void* ElementOf(Doc& d, const void* e, const char* name)
{
    const void* a = FindAttr(e, name);
    if (!a || AttrType(a) != kAtElement) return nullptr;
    const void* data = At<const void*>(a, kAttrData);
    return data ? d.getElement(d.dm, At<uint32_t>(data, 0)) : nullptr;
}

int ArrayCount(const void* a)
{
    const void* v = At<const void*>(a, kAttrData);
    return v ? At<int>(v, 0) : 0;
}

void* ArrayElement(Doc& d, const void* a, int i)
{
    const void* v = At<const void*>(a, kAttrData);
    const uint32_t* h = At<const uint32_t*>(v, 8);
    return h ? d.getElement(d.dm, h[i]) : nullptr;
}

const char* NameOf(const void* e) { return e ? StringOf(e, "name") : "(none)"; }


// The datamodel, checked against the mapped build first.
bool OpenDoc(Doc& d, char* summary, int size)
{
    summary[0] = 0;
    HMODULE sfm = GetModuleHandleA("sfm.dll");
    if (!sfm) { sprintf_s(summary, size, "sfm.dll is not loaded"); return false; }
    uint8_t* base = (uint8_t*)sfm;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kStamp2022) {
        sprintf_s(summary, size, "sfm.dll build 0x%08X is not the one mapped (6 May 2022)", nt->FileHeader.TimeDateStamp);
        return false;
    }
    d = Doc();
    d.base = base;
    d.dm = *(void**)(base + kDataModelPtr);
    if (!d.dm || *(void**)d.dm != base + kDataModelVtable) {
        sprintf_s(summary, size, "the datamodel is not where it was mapped");
        return false;
    }
    void** vt = *(void***)d.dm;
    d.getElement = (GetElementFn)vt[kVfGetElement];
    d.typeName = (TypeNameFn)vt[kVfTypeName];
    return true;
}


// --- The camera's SSAO settings, for RTAO.
//
// S2FM's DmeCamera carries SSAOStrength, SSAOBias and SSAORadius and never applies them: it hands the
// renderer r_ssao's defaults instead (sfm.dll 0x5C1E50). Read here for our AO, from the camera of the
// shot the playhead is in (the first shot's when none is). Called from the render thread a few times a
// second while S2FM edits on Qt's, so every read is inside one __try, and the session, once found, is
// only looked for again when its handle stops giving it.

// CMovieDoc and its GetHeadTime.
const uint32_t kMovieDocPtr = 0x8EC508, kMovieDocVtable = 0x793228, kGetHeadTime = 0x1D21A0;
const uint8_t kGetHeadTimeStart[6] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
typedef int* (__fastcall* GetHeadTimeFn)(void* doc, int* out);
const int kAtFloat = 3, kAtTime = 7;
uint32_t g_ssaoSession = 0;

bool FloatOf(const void* e, const char* name, float* out)
{
    const void* a = FindAttr(e, name);
    if (!a || AttrType(a) != kAtFloat) return false;
    const void* data = At<const void*>(a, kAttrData);
    if (!data) return false;
    *out = At<float>(data, 0);
    return true;
}

bool TimeOf(const void* e, const char* name, int* out)
{
    const void* a = e ? FindAttr(e, name) : nullptr;
    if (!a || AttrType(a) != kAtTime) return false;
    const void* data = At<const void*>(a, kAttrData);
    if (!data) return false;
    *out = At<int>(data, 0);
    return true;
}

void* FindSession(Doc& d)
{
    const int count = At<int>(d.dm, kHandleCount);
    const uint8_t* table = At<const uint8_t*>(d.dm, kHandleTable);
    for (int i = 0; i < count && table; ++i) {
        const uint8_t* entry = table + (size_t)i * kHandleEntry;
        if (At<uint32_t>(entry, 0) & 0x80000000u) continue;
        void* e = At<void*>(entry, 8);
        if (e && !strcmp(TypeOf(e), "DmElement") && FindAttr(e, "activeClip") && FindAttr(e, "clipBin")) return e;
    }
    return nullptr;
}

// Why the camera's settings were not read, for the log.
const char* g_ssaoWhy = "";

bool CameraSsao(Doc& d, float* radius, float* bias, float* strength, char* name, int nameSize)
{
    g_ssaoWhy = "";
    void* session = g_ssaoSession ? d.getElement(d.dm, g_ssaoSession) : nullptr;
    if (!session || strcmp(TypeOf(session), "DmElement") || !FindAttr(session, "activeClip")) {
        session = FindSession(d);
        g_ssaoSession = session ? At<uint32_t>(session, kElemHandle) : 0;
    }
    if (!session) { g_ssaoWhy = "no session is open"; return false; }
    void* movie = ElementOf(d, session, "activeClip");
    if (!movie) { g_ssaoWhy = "the session has no movie"; return false; }
    // The playhead, in ticks of a ten thousandth of a second, when the movie document is where it was
    // mapped; without it the first shot.
    int head = 0;
    bool haveHead = false;
    void* doc = *(void**)(d.base + kMovieDocPtr);
    if (doc && *(void**)doc == d.base + kMovieDocVtable && !memcmp(d.base + kGetHeadTime, kGetHeadTimeStart, 6)) {
        // GetHeadTime gives the time through the pointer it returns, not through its argument: read through
        // the argument, the head was always 0 and the first shot was taken.
        const int* t = ((GetHeadTimeFn)(d.base + kGetHeadTime))(doc, &head);
        haveHead = t != nullptr;
        if (t) head = *t;
    }
    void* shot = nullptr;
    void* first = nullptr;
    void* group = ElementOf(d, movie, "subClipTrackGroup");
    const void* tracks = group ? FindAttr(group, "tracks") : nullptr;
    for (int t = 0; tracks && t < ArrayCount(tracks) && !shot; ++t) {
        void* track = ArrayElement(d, tracks, t);
        const void* children = track ? FindAttr(track, "children") : nullptr;
        for (int c = 0; children && c < ArrayCount(children) && !shot; ++c) {
            void* child = ArrayElement(d, children, c);
            if (!child || strcmp(TypeOf(child), "DmeFilmClip")) continue;
            if (!first) first = child;
            void* tf = ElementOf(d, child, "timeFrame");
            int start = 0, duration = 0;
            if (haveHead && TimeOf(tf, "start", &start) && TimeOf(tf, "duration", &duration)
                && head >= start && head < start + duration) shot = child;
        }
    }
    if (!shot) shot = first;
    if (!shot) { g_ssaoWhy = "the movie has no shot"; return false; }
    void* camera = ElementOf(d, shot, "camera");
    if (!camera) { g_ssaoWhy = "the shot has no camera"; return false; }
    if (!FloatOf(camera, "SSAORadius", radius) || !FloatOf(camera, "SSAOBias", bias)
        || !FloatOf(camera, "SSAOStrength", strength)) {
        g_ssaoWhy = "the shot's camera has no SSAORadius, SSAOBias or SSAOStrength";
        return false;
    }
    strncpy_s(name, nameSize, NameOf(camera), _TRUNCATE);
    return true;
}

bool CameraSsaoGuarded(Doc& d, float* radius, float* bias, float* strength, char* name, int nameSize)
{
    __try {
        return CameraSsao(d, radius, bias, strength, name, nameSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ssaoSession = 0;
        g_ssaoWhy = "reading the document faulted";
        return false;
    }
}

// The playhead (seconds), the render's frame rate and the session's movie name, for the ReShade tool's clock
// (the ReShade package's stage points). The frame rate is the session's settings/renderSettings/frameRate, 24 when absent.
uint32_t g_playSession = 0;

bool Playhead(Doc& d, double* seconds, double* fps, char* name, int nameSize)
{
    void* session = g_playSession ? d.getElement(d.dm, g_playSession) : nullptr;
    if (!session || strcmp(TypeOf(session), "DmElement") || !FindAttr(session, "activeClip")) {
        session = FindSession(d);
        g_playSession = session ? At<uint32_t>(session, kElemHandle) : 0;
    }
    if (!session) return false;
    int head = 0;
    void* doc = *(void**)(d.base + kMovieDocPtr);
    if (!doc || *(void**)doc != d.base + kMovieDocVtable || memcmp(d.base + kGetHeadTime, kGetHeadTimeStart, 6))
        return false;
    const int* t = ((GetHeadTimeFn)(d.base + kGetHeadTime))(doc, &head);   // the time comes through the result
    if (!t) return false;
    head = *t;
    *seconds = head / 10000.0;
    float rate = 24.0f;
    void* settings = ElementOf(d, session, "settings");
    void* render = settings ? ElementOf(d, settings, "renderSettings") : nullptr;
    if (render && FloatOf(render, "frameRate", &rate) && rate > 0.0f && rate < 1000.0f) *fps = rate;
    else *fps = 24.0;
    // The session by its movie's name (the active clip's: what S2FM's title shows). The session element is
    // called "session" in every file, so its own name tells sessions apart from nothing.
    void* movie = ElementOf(d, session, "activeClip");
    if (name && nameSize > 0) strncpy_s(name, nameSize, movie ? NameOf(movie) : NameOf(session), _TRUNCATE);
    return true;
}

bool PlayheadGuarded(Doc& d, double* seconds, double* fps, char* name, int nameSize)
{
    __try {
        return Playhead(d, seconds, fps, name, nameSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_playSession = 0;
        return false;
    }
}

}   // namespace

// Has S2FM draw the view again (and start refining again) without moving anything: the playhead is jumped a
// tick on and back, as a user's change (CMovieDoc::JumpToTime, 0x1D27D0). For the
// ReShade panel, whose changes the engine knows nothing of. S2FM's main thread only.
namespace {
const uint32_t kJumpToTime = 0x1D27D0;
const uint8_t kJumpToTimeStart[7] = { 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20 };
typedef void (__fastcall* JumpToTimeFn)(void* doc, int ticks, bool userChange, bool b, bool c);

bool RedrawGuarded(Doc& d)
{
    __try {
        void* doc = *(void**)(d.base + kMovieDocPtr);
        if (!doc || *(void**)doc != d.base + kMovieDocVtable || memcmp(d.base + kGetHeadTime, kGetHeadTimeStart, 6)
            || memcmp(d.base + kJumpToTime, kJumpToTimeStart, 7)) return false;
        int head = 0;
        const int* t = ((GetHeadTimeFn)(d.base + kGetHeadTime))(doc, &head);
        if (!t) return false;
        head = *t;
        ((JumpToTimeFn)(d.base + kJumpToTime))(doc, head + 1, true, false, false);
        ((JumpToTimeFn)(d.base + kJumpToTime))(doc, head, true, false, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}   // namespace

bool SfmRedraw()
{
    char why[160];
    Doc d;
    if (!OpenDoc(d, why, sizeof(why))) return false;
    return RedrawGuarded(d);
}

bool SfmPlayhead(double* seconds, double* fps, char* sessionName, int nameSize)
{
    char why[160];
    Doc d;
    if (!OpenDoc(d, why, sizeof(why))) return false;
    return PlayheadGuarded(d, seconds, fps, sessionName, nameSize);
}

// The SSAO settings of the camera the shot at the playhead looks through. False when there is no
// session, shot or camera, or a read faulted.
bool SfmCameraSsao(float* radius, float* bias, float* strength, char* cameraName, int nameSize)
{
    // Each reason it cannot be read is logged once, when it starts.
    static char said[160] = "";
    char why[160];
    Doc d;
    bool ok = OpenDoc(d, why, sizeof(why));
    if (ok) {
        ok = CameraSsaoGuarded(d, radius, bias, strength, cameraName, nameSize);
        strcpy_s(why, g_ssaoWhy);
    }
    if (ok) {
        if (said[0]) Log("[DOC] the camera's SSAO settings are read again");
        said[0] = 0;
    } else if (strcmp(why, said) != 0) {
        strcpy_s(said, why);
        Log("[DOC] the camera's SSAO settings are not read (RTAO uses its defaults): %s", why);
    }
    return ok;
}

