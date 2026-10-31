#pragma once
// What touches a water and what it leaves behind (water_fx.cpp).
#include "../core/core.h"
#include "scene_depth.h"

// Before a water's draw: notes its view and gives the effects map to bind where it reads g_tWaterEffects (null when
// the map cannot be made).
ID3D11ShaderResourceView* WaterFxBefore(ID3D11DeviceContext* ctx, ContextState* st);
// After it: st is the draw's state (drawn again, depth only, for the water's own depth), sceneDepth the depth
// before it (SceneDepthFor's copy 0), copy the frame copy it read (copyGen its generation); the map takes its steps,
// the splashes fly, and they are drawn once for each frame copy.
void WaterFxAfter(ID3D11DeviceContext* ctx, const ContextState* st, ID3D11ShaderResourceView* sceneDepth,
                  ID3D11ShaderResourceView* copy, uint64_t copyGen);
// The wetness when no water is in the view (a wet model the camera turned to, the water behind it). Every engine
// draw: the scene's opaque draws note its view and targets (once a water was ever drawn).
void WaterFxSceneDraw(ID3D11DeviceContext* ctx, ContextState* st);
// Whether the frame's scene was noted, and whether its wetness is still to be drawn (no water drew it this frame).
bool WaterFxSceneNoted();
bool WaterFxWetPending();
// The scene's colour target and depth noted (not held for the caller: valid until the frame's end).
void WaterFxSceneTargets(ID3D11RenderTargetView** rtv, ID3D11Texture2D** depth);
// At the frame's end, the first draw of a pass after the scene (bloom, post processing...): when still to be drawn,
// the map stepped and the wetness drawn into the noted target with copy (the picture's copy, copyGen its generation)
// and sceneDepth (its depth, one sample); then the next frame's. Everything bound is put back.
void WaterFxFrameEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* copy, uint64_t copyGen,
                     ID3D11ShaderResourceView* sceneDepth);
// Test words: "noend" (no wetness at the frame's end), "wettrace" (the next 40 events of the wetness's frame logged:
// the frame ends, the waters' draws, the passes after the scene; WaterFxTracing is true while some are left, and
// takes one).
void WaterFxNoEnd(bool off);
// Parts of the wet look left out, to tell which does what: 1 the mirror ("wetnomirror"), 2 the light around where
// there is no mirror ("wetnoaround"), 4 the shine of what is lit ("wetnoshine"), 16 all that is added ("wetnoadd"),
// 32 all the darkening ("wetnomul"); 64 the test view of where a moving model is taken ("modeldebug"), 128 the
// picture it reads under each pixel ("copydebug").
void WaterFxLookOff(unsigned parts);
void WaterFxTrace(bool arm);
bool WaterFxTracing();
// A 4 x 4 matrix's inverse; false when it has none.
bool Inverse4x4(const float* m, float* out);
// The test view of the wetness pass: wet red, the water's height known blue, the rest dimmed.
void WaterFxDebug(bool on);
