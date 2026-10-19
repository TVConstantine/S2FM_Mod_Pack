#pragma once
// What goes into a stage and comes out of it (plan tasks 4 and 5). Depth: the view distance the proxy gives,
// turned into the value ReShade.fxh's own formula (and MartysMods' mmx_depth.fxh, the same formula) turns back
// into exactly our linear depth, L = z / (N*F):
//     those headers compute L = d / (F - d*(F - 1)), F = RESHADE_DEPTH_LINEARIZATION_FAR_PLANE,
//     so the core hands them d = L*F / (1 + L*(F - 1)).
// No header is changed. N and F default to SOURCE2_NEAR_PLANE 12 and FAR_PLANE 2000000, the values Source 2
// presets for ReShade at Present use, so those presets keep their tuning.
#include <d3d11.h>
#include <stdint.h>
#include "rs_stage_api.h"
#include <string>
#include <utility>
#include <vector>

namespace rs {

struct depth_scale {
    double nearPlane = 12.0;      // SOURCE2_NEAR_PLANE
    double farPlane = 2000000.0;  // RESHADE_DEPTH_LINEARIZATION_FAR_PLANE
};
// From the definitions every effect gets (the defaults when one is missing).
depth_scale depth_scale_from(const std::vector<std::pair<std::string, std::string>>& definitions);

// viewZ: R32F view distance at w x h (0, or 1e30 and above, is sky). Writes the encoded depth into the core's own
// depth texture, which depth_srv() gives (made or remade at w x h).
bool depth_encode(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* viewZ, uint32_t w, uint32_t h, depth_scale s);
ID3D11ShaderResourceView* depth_srv();
void depth_release();

// The engine's picture into the back buffer (RGBA8, sRGB-encoded values, the size of the picture), and the
// back buffer out again:
//   A  the 8x MSAA scene, resolved, times the exposure, through x/(1+x) and the sRGB encode;
//   B  post_process's linear output, resolved (its samples are equal), through the sRGB encode;
//   C  the 8-bit picture's bytes as they are.
// A and B write back only what the effects changed: the back buffer as the copy in left it is kept, and where an
// effect changed a pixel its samples move by what takes their resolve to the effect's value (each keeps its own
// offset, so an edge keeps its coverage); a pixel no effect touched is left exactly as it was. C writes the back
// buffer's value, which the 8-bit target takes back byte for byte where nothing changed.
bool stage_copy_in(ID3D11DeviceContext* ctx, const DxrStageFrame& f, ID3D11RenderTargetView* back);
bool stage_copy_out(ID3D11DeviceContext* ctx, const DxrStageFrame& f, ID3D11ShaderResourceView* back);
void stage_io_release();

}  // namespace rs
