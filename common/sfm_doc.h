#pragma once
// S2FM's open document (the 6 May 2022 sfm.dll only; false on any other build or when nothing is open).

// The playhead in seconds, the render's frame rate (24 when unset) and the session's movie name (what S2FM's title
// shows; sessions are told apart by it).
bool SfmPlayhead(double* seconds, double* fps, char* sessionName, int nameSize);
// Has S2FM draw the view again without moving anything (the playhead jumped a tick on and back). S2FM's main thread.
bool SfmRedraw();
// The SSAO sliders of the camera the shot at the playhead looks through.
bool SfmCameraSsao(float* radius, float* bias, float* strength, char* cameraName, int nameSize);

// The fog controls S2FM's "Add Fog Controls To Camera" puts on that camera (sfm.dll adds fogenable, fogcolor,
// fogstart, fogend, fogmaxdensity and fogexponent, each with an animation control): their values at the playhead.
// The colour is 0..1 as the colour picker has it (sRGB). False when the camera has none of them (the command was
// never run on it) or nothing could be read.
struct SfmFog {
    bool enabled;
    float color[3];
    float start, end, maxDensity, exponent;
};
bool SfmCameraFog(SfmFog* fog, char* cameraName, int nameSize);

// The lights of the shot at the playhead with what they give fog: a projected light its volumetricIntensity when
// volumetric is on, 0 when off; a point, spot or orthographic light its fogContributionStrength. And the light's
// world position, which tells it apart in the engine's light buffers: a point, spot or orthographic light's as S2FM
// hands it to the engine (right under a moving group too), a projected light's its transform's (the document's, right
// only for one that hangs under nothing). Hidden lights are left out. The count written, or -1 when nothing could be
// read.
struct SfmLightFog {
    float position[3];
    float weight;
    bool projected;    // a projected light (S2FM's own light list), else one of the engine's light list
    bool fromEngine;   // the position from what S2FM hands the engine, not the document
    bool ortho;        // an orthographic light: the engine's list has it far back along its forward
    float forward[3];  // where it points (from what S2FM hands the engine), for an orthographic light
    const void* engine;  // what S2FM hands the engine for it (SfmLightNow), null for a projected light
    float radius;        // its size, S2FM's radius (0: a point), for the soft highlights of a shader compiler's surfaces
    char name[48];
};
int SfmLightFogs(SfmLightFog* out, int max);
// A light's position and forward now, from what S2FM hands the engine (SfmLightFog::engine): S2FM writes them every
// frame, so a light that moves with an animation is read where it is at the draw. False when it cannot be read.
bool SfmLightNow(const void* engine, float position[3], float forward[3]);

// Puts the weather volumes before the see-through draws and particles they would otherwise cover: the engine sorts
// those back to front by the nearest point of their bounds, so a volume round the camera comes last. Each model of the
// shot at the playhead whose material override (its materials, or its replacement) isWeather says yes to gets bias as
// its scene object's depth sort bias (more is farther, drawn earlier); a model that had it from here and stopped being
// a weather volume gets 0. Not an S2FM attribute: never saved, made again when S2FM makes the scene object again.
// Writes out the weather models (and any model given its bias back); the count, or -1 when nothing could be read.
struct SfmSortedModel {
    char name[48];
    float bias;       // the bias it has now
    bool written;     // changed by this call
    bool noObject;    // a weather model without a scene object to write to (hidden, or not made yet)
};
int SfmWeatherSortBias(bool (*isWeather)(const char* material), float bias, SfmSortedModel* out, int max);
