#pragma once
// S2FM's open document (the 6 May 2022 sfm.dll only; false on any other build or when nothing is open).

// The playhead in seconds, the render's frame rate (24 when unset) and the session's movie name (what S2FM's title
// shows; sessions are told apart by it).
bool SfmPlayhead(double* seconds, double* fps, char* sessionName, int nameSize);
// Has S2FM draw the view again without moving anything (the playhead jumped a tick on and back). S2FM's main thread.
bool SfmRedraw();
// The SSAO sliders of the camera the shot at the playhead looks through.
bool SfmCameraSsao(float* radius, float* bias, float* strength, char* cameraName, int nameSize);
