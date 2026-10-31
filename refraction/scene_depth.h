#pragma once
// The scene's depth for a see-through surface of a shader compiler's own (scene_depth.cpp).
#include <d3d11.h>
#include <cstdint>

// The depth bound now, one sample, as a view the pixel shader can read; made again when gen changes (the frame copy's
// count, so it is made with the picture's copy). which picks one of three kept copies (0: a surface's own, 1: the
// depth after a water's draw, 2: the wetness at the frame's end). Null when the bound depth cannot be read.
ID3D11ShaderResourceView* SceneDepthFor(ID3D11DeviceContext* ctx, uint64_t gen, int which = 0);
// The same of a depth texture that is not bound: at the frame's end, where binding the scene's depth would take it
// from the engine's pass that reads it.
ID3D11ShaderResourceView* SceneDepthOf(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint64_t gen, int which);
