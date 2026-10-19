#pragma once
// RTAO: ray traced ambient occlusion in S2FM's own viewports (docs/rtao.md).
//   rtao_engine.cpp   the engine's side: the view, its depth, where the AO goes on and how (ported from the main
//                     project's engine viewport AO), and the feature on the core;
//   rtao_capture.cpp  the geometry: each opaque draw of the view drawn once more with stream output, and the cut outs'
//                     uv, alpha and masks;
//   rtao_d3d12.cpp    the rays: a D3D12 device beside the engine's, the structure and the AO.
#include <d3d11_4.h>
#include <d3d12.h>
#include <stdint.h>
#include <vector>

// The masks the cut outs are judged by, at most this many textures a frame (rtao_capture.cpp).
#define RTAO_MASKS 32

// A cut out of the frame: its triangles in the capture (three vertices each, from firstVertex), the mask of its alpha,
// and how it cuts.
struct RtaoCutout {
    UINT firstVertex, vertexCount;
    UINT mask;          // which of the RTAO_MASKS
    float reference;    // its alpha at or over this blocks a ray; below zero: it blocks with the chance of its alpha
    UINT vertexAlpha;   // 1: the alpha times the vertex's own (the third value captured beside the uv)
};

// The views the frame's triangles were drawn with (rtao_capture.cpp): the camera's (always the first) and the others
// the engine draws before it, the shadows' among them, which hold what is off the screen. At most RTAO_VIEWS a frame,
// and the capture in at most RTAO_RUNS runs of one view each.
#define RTAO_VIEWS 16
#define RTAO_RUNS 4096
// A view as the rays take it: from its clip space to the rays' space (clip times clipToRays, divided by w, plus
// delta), and whether its triangles are left out where the camera has them or they are out of the AO's reach.
struct RtaoView {
    float clipToRays[16];
    float delta[3];
    UINT cull;
};
struct RtaoRun {
    UINT firstVertex, view;
    UINT flags;   // 1: the vertices' smooth normals were captured (RtaoNormalBuffer)
};
// A view as the capture took it: its world to projection matrix and its world to camera offset.
struct RtaoCaptureView {
    float worldToProj[16];
    float offset[3];
};

// --- rtao_d3d12.cpp
// D3D12 on the engine's adapter, with DXR tier 1.1. False when it cannot be (said once; RTAO then stays off).
bool RtaoD3D12Start(ID3D11Device* dev);
bool RtaoD3D12Ready();
// The engine side's slots were released: the views made for them are forgotten. A new texture can come back at a
// freed one's address, and views kept by address then pointed at freed memory (the driver reset).
void RtaoSlotsChanged();
// A texture made on the engine's device, shared, opened on ours. Null when it cannot be.
ID3D12Resource* RtaoOpenTexture(ID3D11Texture2D* tex11);
// The capture buffer (stream output on the engine's side, read on ours), made or made again at bytes or more.
// *out11 is the engine's side, valid until the next call that grows it. The uv buffer the same, for the cut outs.
bool RtaoCaptureBuffer(ID3D11Device* dev, UINT bytes, ID3D11Buffer** out11, UINT* madeBytes);
bool RtaoUvBuffer(ID3D11Device* dev, UINT bytes, ID3D11Buffer** out11, UINT* madeBytes);
// The vertices' smooth normals (the vertex shader's own, 12 bytes a vertex, at the same vertex as its position).
bool RtaoNormalBuffer(ID3D11Device* dev, UINT bytes, ID3D11Buffer** out11, UINT* madeBytes);
// The two fences, on the engine's side: the capture and depth copy done (signalled by the engine's context, waited on
// by our queue), and the AO done (the other way round).
ID3D11Fence* RtaoCapturedFence11();
ID3D11Fence* RtaoAoFence11();
// Whether our queue has finished the AO sent with that fence value.
bool RtaoAoDone(UINT64 value);

struct RtaoFrame {
    float projToRays[16];   // inverse of the frame's world to projection matrix (row vectors)
    float eye[3];           // the camera in the rays' space (its position plus the world to camera offset)
    float reach, bias, strength;
    UINT rays;
    UINT width, height;     // the pixels to trace
    UINT seed;
    float minDepth, maxDepth;
    UINT vertices;          // captured this frame, three a triangle
    uint64_t drawList;      // the frame's draws (rtao_capture.cpp): the same list lets the structure be refitted
    bool debug;
    bool noRefit;           // "norefit": the bottom structure made afresh every frame
    ID3D12Resource* depth;  // R32_FLOAT, the view's depth
    ID3D12Resource* out;    // R16G16B16A16_FLOAT, the AO in r
    int slot;               // which of the two depth and AO pairs (their views are kept per slot)
    const RtaoCutout* cutouts;   // the frame's cut outs, in capture order
    UINT cutoutCount;
    ID3D12Resource* masks[RTAO_MASKS];
    float worldToClip[16];  // the camera's world to projection matrix (row vectors), for the other views' culling
    const RtaoView* views;  // the first the camera's
    UINT viewCount;
    const RtaoRun* runs;    // in capture order, the first from vertex 0
    UINT runCount;
};
// Our queue waits for the captured fence to reach capturedValue, makes the frame's structure from the captured
// vertices, traces the AO into f.out and signals the AO fence with aoValue. False when nothing was sent.
bool RtaoTrace(const RtaoFrame& f, UINT64 capturedValue, UINT64 aoValue);
// For the panel: the last frame's triangles, what the structure holds on the card, and why RTAO cannot run ("" while
// it can).
void RtaoD3D12Info(long long* triangles, float* structureMb, const char** why);
// For the 10 s line: the card's mean time a frame (ms) for the conversion, the bottom structure, the top one and the
// AO, since the last call.
void RtaoD3D12Times(double ms[4]);
// The same over a line of the test words (rtao_engine.cpp's out file), and the structures made and refitted then.
void RtaoD3D12LineTimes(double ms[4], long long* builds, long long* refits);
// Bottom structures made afresh and refitted since the last call.
void RtaoD3D12Builds(long long* builds, long long* refits);
// For the 10 s line: frames traced, the last frame's triangles, and the structure's sizes in MB.
void RtaoD3D12Stats(long long* traced, long long* triangles, float* structureMb);

// --- rtao_capture.cpp
// At the frame's first AO reader: an empty capture for the frame, whose rays go from this camera (the frame's world to
// projection matrix; draws drawn with another are not taken).
void RtaoCaptureBegin(const float worldToProj[16]);
// "nocutouts" on the test words: the draws whose pixel shader discards are left out of the rays.
void RtaoCaptureLeaveCutouts(bool leave);
// "solidcutouts": they are traced as solid, as before their alpha was followed.
void RtaoCaptureSolidCutouts(bool solid);
// "flatnormals": no smooth normals captured; the AO is turned by the faces, as before.
void RtaoCaptureFlatNormals(bool flat);
// After an AO reader of the view is drawn: it again, with stream output, into the capture.
void RtaoCaptureDraw(ID3D11DeviceContext* ctx, struct ContextState* st);
// Before the frame's first AO reader: a depth only draw of another view (the shadows'), into the capture of the frame
// to come, with its own view. Only with "offscreen" on the test words (off by default: docs/rtao.md).
void RtaoCaptureOther(ID3D11DeviceContext* ctx, struct ContextState* st);
void RtaoCaptureLeaveOthers(bool leave);
// At a present: other views' draws taken with no frame of the camera after them are let go.
void RtaoCaptureDrop();
// The frame's views (the first, the camera's, left empty here) and runs, valid until the next capture opens.
const std::vector<RtaoCaptureView>& RtaoCaptureViewList();
const std::vector<RtaoRun>& RtaoCaptureRunList();
// Other views' draws and triangles captured since the last call.
void RtaoCaptureOthers(long long* draws, long long* triangles);
// The vertices captured this frame (three a triangle), closing the capture.
UINT RtaoCaptureVertices();
// The frame's cut outs (valid until the next frame's capture begins) and the masks' textures on our device.
const std::vector<RtaoCutout>& RtaoCaptureCutoutList();
void RtaoCaptureMasks(ID3D12Resource* out[RTAO_MASKS]);
// What the frame's draws were (their shaders and counts, in order): the same as the frame before's means the same
// triangles in the same order, moved at most.
uint64_t RtaoCaptureDrawList();
void RtaoCaptureStats(long long* taken, long long* skipped, long long* overflow);
// Cut outs captured since the last call: draws and triangles.
void RtaoCaptureCutouts(long long* draws, long long* triangles);
