#include "rs_vswap.h"

namespace {
ID3D11Texture2D* MakeBackBuffer(ID3D11Device* d3d, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    // 8-bit, as a game's back buffer is: effects that sample it through an sRGB view (MagicHDR, SMAA) get
    // one, BUFFER_COLOR_BIT_DEPTH is 8, and presets tuned on ReShade at Present see what they did.
    // Half floats had no sRGB view, and MagicHDR, fed encoded values as if linear, washed the picture out.
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(d3d->CreateTexture2D(&d, nullptr, &t))) return nullptr;
    return t;
}
}  // namespace

rs::virtual_swapchain::virtual_swapchain(reshade::api::device* device, ID3D11Device* d3d, uint32_t width,
                                         uint32_t height)
    : api_object_impl(MakeBackBuffer(d3d, width, height)), _device(device), _width(width), _height(height) {
    if (!_orig) return;
    // Our own views are plain UNORM: the stages copy sRGB-encoded values in and out as they are.
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    d3d->CreateRenderTargetView(_orig, &rd, &_rtv);
    // And the sRGB one ReShade draws through for a pass that asks for SRGBWriteEnable.
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    d3d->CreateRenderTargetView(_orig, &rd, &_rtvSrgb);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    d3d->CreateShaderResourceView(_orig, &sd, &_srv);
}

rs::virtual_swapchain::~virtual_swapchain() {
    if (_srv) _srv->Release();
    if (_rtvSrgb) _rtvSrgb->Release();
    if (_rtv) _rtv->Release();
    if (_orig) _orig->Release();
}

reshade::api::resource rs::virtual_swapchain::get_back_buffer(uint32_t) {
    return { reinterpret_cast<uintptr_t>(static_cast<ID3D11Resource*>(_orig)) };
}
