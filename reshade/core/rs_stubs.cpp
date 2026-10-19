// What ReShade's runtime links against for the graphics APIs this core does not build (OpenGL's GLSL and
// Vulkan's SPIR-V code generators). The runtime only asks for them on those APIs; the core runs on D3D11,
// which takes the DXBC generator (effect_codegen_dxbc.cpp).
#include "effect_codegen.hpp"

reshadefx::codegen* reshadefx::create_codegen_glsl(bool, bool, bool, bool, bool) { return nullptr; }
reshadefx::codegen* reshadefx::create_codegen_spirv(bool, bool, bool, bool, bool) { return nullptr; }

// Input: the core has no window of its own and takes no keys (the panel is the way in), so no input object
// is ever made and the runtime skips its shortcuts (it checks for none).
#include "input.hpp"
std::shared_ptr<reshade::input> reshade::input::register_window(window_handle) { return nullptr; }
void reshade::input::next_frame() {}
bool reshade::input::is_keyboard_layout_german() { return false; }
std::shared_ptr<reshade::input_gamepad> reshade::input_gamepad::load() { return nullptr; }
void reshade::input_gamepad::next_frame() {}

// No looking for a newer ReShade on the network.
#include "runtime.hpp"
void reshade::runtime::check_for_update() {}
