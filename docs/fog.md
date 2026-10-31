# Fog

Our own distance fog in S2FM's viewports and renders, driven by the camera's fog controls and independent of the map.

## How it works

1. **Controls.** S2FM's "Add Fog Controls To Camera" adds six controls to the camera: fogenable, fogcolor, fogstart,
   fogend, fogmaxdensity and fogexponent. They are read from the document four times a second (`SfmCameraFog` in
   `common/sfm_doc.cpp`), so their keyframes and animation work as they do for any other control.
2. **Camera.** At the frame's first opaque draw (the first that reads the engine's dynamic AO, the same draw RTAO
   finds) the camera is taken from the view buffer: the inverse of world to projection, the eye, the depth range of
   the viewport and the render targets.
3. **Pass.** At the first draw after the opaque surfaces that is not the sky (or at the first see-through draw
   before it) the scene's depth is copied and one full screen triangle blends the fog over the picture, per sample
   on a multisampled target. The distance is the true 3D distance from the eye. The sky is fogged too.
4. **Formula.** fog = maxdensity x saturate((distance - start) / (end - start)) ^ exponent.
5. **Colour.** S2FM's post_process applies a filmic tone map after the scene. The fog colour is that curve solved
   backwards, so the picture shows the colour the camera asks for. The curve's constants come from the last
   post_process draw; without one, a plain x / (1 + x) is used.
6. **The engine's own camera fog** is broken: it greys the sky through the volumetric fog and fogs next to nothing
   else. While ours is on it is switched off by a hook in sfm.dll (fn 0x1F73A0, 6 May 2022 build only, checked by
   time stamp and prologue). The camera's controls themselves are not changed.

Order in the frame: RTAO, then the fog (the AO never darkens the fog), then ReShade's stages. Stage A runs before the
fog, stages B and C see it.

## Test words

First line of `s2fm_fog_debug.txt` beside the DLL: `off` (no fog), `keepengine` (the engine's camera fog stays on
beside ours), `probe` (logs the settings sfm.dll hands the engine), `dump` (writes the picture after the fog to
`s2fm_fog_picture.ppm`). (Its live test is kept in the development tree, not in this repository.)

## To do

### Missing

- **See-through draws are not fogged.** Glass, particles and additive glows are drawn after the fog pass, so a far
  glowing sprite or window shows at full strength through thick fog. Needs a per draw fog for blended draws (the
  draw's own depth is not in the depth buffer), for example a second pass that fades them by their distance, or
  moving the pass after them and handling their depth.
- **One frame of lag on the tone map.** The tone map constants come from the previous frame's post_process draw. A
  sudden exposure change shows the fog colour off for one frame.
- **Other sfm.dll builds.** On any build but the 6 May 2022 one the hook is not set and the engine's broken camera
  fog stays on beside ours.
- **The map's own fogs** (env_gradient_fog, env_cubemap_fog, volumetric fog) still draw under ours. A switch to turn
  them off for a shot may be wanted.

### Quality

- **Height fog.** Thick near the ground, thinning with height (a density that falls off with world z, integrated
  along the ray so it is right from any camera height).
- **Noise.** Uneven, drifting fog: 3D noise in world space, moved by a wind direction and speed.
- **Light in the fog.** Volumetric scattering from the scene's lights (spot cones and god rays), using the lights'
  shadow maps. The largest step; ray marched at lower resolution and upscaled.
- **More controls.** The six camera controls are all S2FM has. Height, noise and scattering need controls of our own
  on the camera, added the same way and so animatable.
