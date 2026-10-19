// ReShade's state_block.cpp dispatches to every graphics API it supports; the core is D3D11 only, so this
// is that file with the D3D11 branch alone (the others would pull in OpenGL's and D3D9's headers).
#include "state_block.hpp"
#include "d3d11/d3d11_impl_state_block.hpp"

void reshade::api::create_state_block(api::device* device, state_block* out_state_block) {
    *out_state_block = { device->get_api() == api::device_api::d3d11
        ? reinterpret_cast<uintptr_t>(new d3d11::state_block(reinterpret_cast<ID3D11Device*>(device->get_native())))
        : 0 };
}
void reshade::api::destroy_state_block(api::device* device, state_block state_block) {
    if (device->get_api() == api::device_api::d3d11) delete reinterpret_cast<d3d11::state_block*>(state_block.handle);
}
void reshade::api::apply_state(api::command_list* cmd_list, state_block state_block) {
    if (cmd_list->get_device()->get_api() == api::device_api::d3d11 && state_block.handle)
        reinterpret_cast<d3d11::state_block*>(state_block.handle)->apply_and_release();
}
void reshade::api::capture_state(api::command_list* cmd_list, state_block state_block) {
    if (cmd_list->get_device()->get_api() == api::device_api::d3d11 && state_block.handle)
        reinterpret_cast<d3d11::state_block*>(state_block.handle)->capture(
            reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()));
}
