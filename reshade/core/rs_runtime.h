#pragma once
// The core's ReShade runtime: made on the engine's device and immediate context with a virtual swapchain at
// the picture's size, driven a stage at a time on S2FM's clock (reshade/patches/0001). Plan task 3.
#include <d3d11.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "rs_stage_api.h"
#include "rs_assign.h"

namespace reshade { class runtime; }

namespace rs {

struct clock {
    double seconds = 0.0;         // the timer uniform (ms of it)
    uint64_t frameCount = 0;      // the framecount uniform
    uint32_t randomSeed = 0;      // what random draws from
    float frameTimeMs = 1000.0f / 24.0f;
};

// configPath: the ReShade.ini-format file the runtime reads (and may write back): a copy the core made.
bool core_init(ID3D11Device* dev, ID3D11DeviceContext* immediate, uint32_t width, uint32_t height,
               const std::string& configPath, const std::vector<std::pair<std::string, std::string>>* definitions);
void core_shutdown();
// ReShade's log (s2fm_reshade.log beside the core), opened once.
void core_open_log();
bool core_ready();                          // made and initialised
uint32_t core_width();
uint32_t core_height();
bool core_loading();                        // effects still compiling or being made
// Lets loading progress (it is finished on the render thread): returns once loaded or after timeoutMs.
bool core_wait_loaded(uint32_t timeoutMs);
// One step of loading on the render thread (what is compiled is made), without drawing.
void core_pump();
// The back buffer the stages draw on.
ID3D11Texture2D* core_back_buffer();
ID3D11RenderTargetView* core_back_rtv();
ID3D11ShaderResourceView* core_back_srv();
// One stage on the back buffer as it is: the frame's clock, then the techniques set in 'techniques'.
bool core_run_stage(const clock& c, const std::vector<bool>& techniques);
// A whole stage: the engine's picture in (rs_stage_io), the depth bound when given, the techniques run, the
// picture out; the engine's state saved before and put back after. False when the picture's size is not the
// runtime's (it is made again at the new size by the caller) or nothing could be drawn.
bool core_stage(const DxrStageFrame& f, const clock& c, const std::vector<bool>& techniques);
// What a stage costs on the card, smoothed (ms), 0 until measured.
double core_stage_ms(int stage);
// The techniques, in the preset's order: "Name@file.fx", and whether enabled.
size_t core_technique_count();
std::vector<std::string> core_technique_names(std::vector<bool>* enabled, std::vector<size_t>* indices);
// The filter for a stage from a comma list of "Name@file.fx" or "Name"; "" = every technique, "-" = none.
std::vector<bool> core_filter_from_names(const std::string& list);
// What the runtime said went wrong compiling, all effects.
std::string core_errors();
// Binds a texture to a semantic in every effect (DEPTH).
void core_bind_semantic(const char* semantic, ID3D11ShaderResourceView* srv);
// The runtime itself, for the panel's API (under rs::api_mutex()); null when not running.
reshade::runtime* core_runtime();
// What each technique is (rs_assign.h), in the preset's order, with the runtime's index of each.
std::vector<technique_facts> core_technique_facts(std::vector<size_t>* indices);
// Effects on or off: off, a stage draws nothing.
void core_set_effects_on(bool on);
// A texture an effect declared, read back as floats (4 a texel) when its format allows; for the tests.
bool core_read_effect_texture(const char* name, std::vector<float>& out, uint32_t* w, uint32_t* h);

}  // namespace rs
