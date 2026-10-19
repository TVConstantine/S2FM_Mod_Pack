#pragma once
// RTAO's settings and state, exported by the package's dxgi.dll for the ReShade tool's panel (its RTAO switch and
// rays). Plain C types only: the panel is built with /Zc:wchar_t- for Qt 4. The panel looks the exports up in the
// dxgi.dll the game loaded; a dxgi.dll without them has no RTAO, and the panel shows none.
//
// The settings are kept in s2fm_rtao.ini beside the DLL (Enabled, Rays) and read when the game starts.

#define S2FM_RTAO_API_VERSION 1

struct S2fmRtaoState {
    int apiVersion;      // S2FM_RTAO_API_VERSION
    int enabled;         // the switch, as saved
    int rays;            // rays a pixel, 1 to 16
    int running;         // the AO went on the picture in the last second
    int unavailable;     // RTAO cannot run here (why says so): no DXR 1.1, D3D12 failed
    long long triangles; // the last frame's structure
    float megabytes;     // what it holds on the card
    float framesPerSecond;
    char why[256];       // why it is not running, when it is not ("" when it is)
};

typedef int(__cdecl* S2fmRtao_Get_Fn)(struct S2fmRtaoState* out);
typedef void(__cdecl* S2fmRtao_Set_Fn)(int enabled, int rays);
