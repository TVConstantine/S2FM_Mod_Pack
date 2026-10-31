#pragma once
// Wetness carried on what moves (water_carry.cpp): every vertex of a skinned model, or of a single object moved by
// its own transform, keeps how far below its wet line it is.
#include "../core/core.h"

// After every engine draw: a skinned model's draw, while a water is in the view, is taken again with stream output.
void WaterCarryDraw(ID3D11DeviceContext* ctx, ContextState* st);
// A water's draw is about to be made into a target of this size (the capture runs for 2 s after the last one).
void WaterCarryWaterSeen(UINT rtW, UINT rtH);
// The frame's scene was drawn into a target of this size (no water in it): its draws are taken while anything is wet.
void WaterCarrySceneSeen(UINT rtW, UINT rtH);
// After a water's draw (every one): the draws taken from here on are the next frame's.
void WaterCarryWaterDone();
// The frame's step, dt seconds at time: each vertex taken this frame wet under the water (the map's g_wet, wetMap),
// drying once out, and dripping now and then into water_fx.cpp's droplets (drops, and ring their counter; null: no
// drips), and its beads of water kept while in the water and going once out. On the compute stage (b0, t0-t2,
// u0-u6), whose bindings the caller puts back.
void WaterCarryStep(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* wetMap, float area, float size, float dt,
                    float time, ID3D11UnorderedAccessView* drops, ID3D11UnorderedAccessView* ring);
// The map started afresh: nothing is wet.
void WaterCarryClear(ID3D11DeviceContext* ctx);
// This frame's models on the screen, drawn where each lies in sceneDepth, at the last water's target size: how wet and
// how much of a bead of water (0 to 1, R16G16F) and their normal (xyz, w 1 where there is one). False, and both
// null, when there are none.
// Everything it binds is put back.
bool WaterCarryScreen(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* sceneDepth, ID3D11ShaderResourceView** wet,
                      ID3D11ShaderResourceView** normal);
// The smooth normals the last WaterCarryScreen drew (w 1 where a moving model is), or null: a frame old.
ID3D11ShaderResourceView* WaterCarryLastNormal();
// "beaddebug" on the test words: every bead of water shown on the moving models, whatever its time.
void WaterCarryBeadView(bool on);
// "nocarry" on the test words: nothing taken, nothing carried.
void WaterCarryOff(bool off);
