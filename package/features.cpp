// What this dxgi.dll brings: each feature registers with the core (core/core.h) once, when the game makes its first
// factory. The order is the order they are called in at each point.
void RefractionRegister();      // refraction/refract.cpp
void WeatherSortRegister();     // refraction/weather_sort.cpp
void RtaoRegister();            // rtao/rtao_engine.cpp
void FogRegister();             // fog/fog.cpp
void ReshadeStagesRegister();   // reshade/stages.cpp
void AssetDepsRegister();       // assets/asset_deps.cpp
void NoWipeRegister();          // refraction/no_wipe.cpp
void SkyBakeRegister();         // sky/sky_bake.cpp

void CoreFeaturesInit() {
    static bool done = false;
    if (done) return;
    done = true;
    // The frame copy before ReShade's stage A, which may come at the same draw: the copy is the engine's picture.
    RefractionRegister();
    // The AO goes on at the first draw after the opaque surfaces, which is where ReShade's stage A runs: before it.
    RtaoRegister();
    // The fog after the AO (which is never to darken it) and after the sky, which comes after stage A.
    FogRegister();
    ReshadeStagesRegister();
    // Not a feature of the frame: the asset system's up-to-date check, which loops the materials of custom shaders.
    AssetDepsRegister();
    // Not a feature of the frame either: the weather volumes' depth sort bias, from S2FM's document.
    WeatherSortRegister();
    // Nor this: the G-Man's "no-wipe" models drawn with the others, so his hair is behind a refracting glass.
    NoWipeRegister();
    // The procedural sky baked into an HDRI when its settings change: its own draw made again into a cube of ours.
    SkyBakeRegister();
}
