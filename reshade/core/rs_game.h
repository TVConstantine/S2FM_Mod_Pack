#pragma once
// The core's state in the game, shared by the stage callback (rs_game.cpp, render thread) and the panel's C
// API (rs_api.cpp, Qt's thread). Everything here is used under api_mutex(), which the render thread holds for
// a whole stage call and the API for each call, so the panel never sees the runtime half way through a frame.
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace rs {

std::recursive_mutex& api_mutex();
bool game_started();
std::filesystem::path game_bin();             // game\bin\win64

// The preset in use: its source (what the panel opened, "" for none) and the copy the runtime reads and
// writes. Using another one, or reloading, makes the runtime again at the next frame.
std::string game_preset_source();
std::filesystem::path game_preset_copy();
void game_use_preset(const std::string& source);
void game_reload();
// Effects on or off (the toolbar's switch), kept across runtimes.
bool game_effects_on();
void game_set_effects_on(bool on);

// Stages, by the runtime's technique index: what is used, what the rules gave, and whether it was set by hand.
// Set by hand is kept in the copy's [S2FM] section as Stage.<technique>=A|B|C.
int game_stage(size_t techniqueIndex, int* autoStage, bool* overridden);
void game_set_stage(const std::string& technique, int stage);   // -1: back to the rules
void game_stages_dirty();

// Has S2FM draw the view again (dxgi.dll's DxrStages_Redraw), after the panel changed something.
void game_redraw();

}  // namespace rs
