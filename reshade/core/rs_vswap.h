#pragma once
// A swapchain with no window for ReShade's runtime: one back buffer the core owns (RGBA8 with sRGB views, colour
// space sRGB non-linear, as a game's), at the picture's size. Each stage copies its engine picture in, the runtime draws on it, and
// the stage copies it back (rs_stage_io).
#include <d3d11.h>
#include "reshade_api_object_impl.hpp"

namespace rs {

class virtual_swapchain final : public reshade::api::api_object_impl<ID3D11Texture2D*, reshade::api::swapchain> {
public:
    virtual_swapchain(reshade::api::device* device, ID3D11Device* d3d, uint32_t width, uint32_t height);
    ~virtual_swapchain();

    bool ok() const { return _orig && _rtv && _rtvSrgb && _srv; }

    reshade::api::device* get_device() final { return _device; }
    void* get_hwnd() const final { return nullptr; }
    reshade::api::resource get_back_buffer(uint32_t index) final;
    uint32_t get_back_buffer_count() const final { return 1; }
    uint32_t get_current_back_buffer_index() const final { return 0; }
    bool check_color_space_support(reshade::api::color_space cs) const final {
        return cs == reshade::api::color_space::srgb_nonlinear;
    }
    reshade::api::color_space get_color_space() const final { return reshade::api::color_space::srgb_nonlinear; }

    ID3D11Texture2D* texture() const { return _orig; }
    ID3D11RenderTargetView* rtv() const { return _rtv; }
    ID3D11RenderTargetView* rtv_srgb() const { return _rtvSrgb; }
    ID3D11ShaderResourceView* srv() const { return _srv; }
    uint32_t width() const { return _width; }
    uint32_t height() const { return _height; }

private:
    reshade::api::device* _device;
    ID3D11RenderTargetView* _rtv = nullptr;
    ID3D11RenderTargetView* _rtvSrgb = nullptr;
    ID3D11ShaderResourceView* _srv = nullptr;
    uint32_t _width, _height;
};

}  // namespace rs
