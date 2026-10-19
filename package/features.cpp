// What this dxgi.dll brings: each feature registers with the core (core/core.h) once, when the game makes its first
// factory. The order is the order they are called in at each point.
void RefractionRegister();      // refraction/refract.cpp
void RtaoRegister();            // rtao/rtao_engine.cpp
void ReshadeStagesRegister();   // reshade/stages.cpp

void CoreFeaturesInit() {
    static bool done = false;
    if (done) return;
    done = true;
    // The frame copy before ReShade's stage A, which may come at the same draw: the copy is the engine's picture.
    RefractionRegister();
    // The AO goes on at the first draw after the opaque surfaces, which is where ReShade's stage A runs: before it.
    RtaoRegister();
    ReshadeStagesRegister();
}
