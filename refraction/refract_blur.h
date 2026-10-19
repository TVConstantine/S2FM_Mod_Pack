#pragma once
// Particle refraction's blur. The particle editor's "refract blur radius" and "refract blur type" (Gaussian, Box) do
// nothing to be seen in the game as it is: particles.dll makes the blur's taps once, when a sprite renderer starts,
// with the radius clamped to 0..4 texels of the frame copy, and its Gaussian divides the distance by the radius but
// not the sigma, so its weights stay between 0.88 and 1, a box (rva 0x11d100 in the 6 May 2022 build). The shader then
// reads at most 25 taps of FilterTaps_t (cb5).
//
// Here that function is hooked: it still makes Valve's taps, and the radius and type the particle asked for are written
// into FilterTaps_t's first register, whose y, z and w the shader never reads. At a refracting draw refract.cpp finds
// them there, blurs the frame copy itself, a true Gaussian (sigma half the radius) or a true box, and hands the shader
// the blurred copy and a single tap. The radius is in pixels of a 1080 line picture, scaled with the picture's height,
// so the viewport and a render of any size look alike. A particle whose renderer started before the hook went on keeps
// Valve's taps.
#include <d3d11.h>
#include <stdint.h>

// After every library load, until particles.dll has been looked at (CoreOnModuleLoad).
void RefractBlurHookIfLoaded();
// The radius and type a refracting particle asked for, from FilterTaps_t's first register. False when the hook did not
// write them (Valve's taps are used as they are).
bool RefractBlurAsked(const float reg0[4], float* radius, int* type);
// The frame copy blurred for that radius and type (copyW x copyH, the picture in its top left), made once per copy
// (copyGen) and radius. Null when there is nothing to blur at this size (the radius under half a pixel) or it could
// not be made.
ID3D11ShaderResourceView* RefractBlurApply(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* copy, UINT copyW,
                                           UINT copyH, UINT pictureW, UINT pictureH, uint64_t copyGen, float radius,
                                           int type);
// "blurdump" on s2fm_refract_debug.txt: at the next blur, the copy brought down and blurred both ways, written beside
// the DLL as s2fm_refract_blur_{down,gauss,box}.pfm.
void RefractBlurDumpArm();
// For the 10 s log line: blurs made and particles seen asking, since the last call.
void RefractBlurStats(int* made, int* asked, float* lastPixels, int* lastType, int* lastReduce);
// A half float's value.
float RefractHalfToFloat(uint16_t h);
