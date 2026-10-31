// The precipitation on the scene's surfaces for s2vfx_weather (the F_SURFACES variant), written from scratch for this
// project: splashes and ripples where it lands, wetness, puddles and snow cover where it gathers, all on the scene
// point where the rain's column meets the scene's depth, with no change to the scene's own materials. The sky light
// (weather_sky.hlsl) keeps it from under roofs and finds the hollows and the tops. A PS block includes this after
// precipitation.hlsl and screen.hlsl, with s_sky (the sky light) and the Volume of weather.vfx.

// Surfaces. Start Time and Fill Time (s, S2FM's time): from Start the surfaces fill toward the amounts below over Fill
// Time (0: full from the start). Wetness: how wet the ground gets (rain, sleet, hail). Darkening: how much darker wet
// ground is. Reflection: how strongly the wet film and the puddles reflect. Puddles: how much of the flat ground the
// puddles cover, the hollows first. Puddle Size: how wide the puddles are, and the hollows they look for. Snow Cover:
// how much snow lies on the tops (snow; sleet a little, hail a scatter). Snow Brightness: how much light the snow sends
// back (fresh snow is near 0.85, but a scene lit for its ground then shows it as flat white).
float g_flSurfaceStart < UiType(Slider); Default(0.0); Range(0.0, 1000.0); UiGroup("Surfaces,87/10"); UiFeature(F_SURFACES); >;
float g_flSurfaceFill < UiType(Slider); Default(0.0); Range(0.0, 600.0); UiGroup("Surfaces,87/20"); UiFeature(F_SURFACES); >;
float g_flWetness < UiType(Slider); Default(1.0); Range(0.0, 1.0); UiGroup("Surfaces,87/30"); UiFeature(F_SURFACES); >;
float g_flDarkening < UiType(Slider); Default(0.4); Range(0.0, 1.0); UiGroup("Surfaces,87/40"); UiFeature(F_SURFACES); >;
float g_flWetReflection < UiType(Slider); Default(1.0); Range(0.0, 2.0); UiGroup("Surfaces,87/50"); UiFeature(F_SURFACES); >;
float g_flPuddles < UiType(Slider); Default(0.4); Range(0.0, 1.0); UiGroup("Surfaces,87/60"); UiFeature(F_SURFACES); >;
float g_flPuddleSize < UiType(Slider); Default(1.0); Range(0.25, 8.0); UiGroup("Surfaces,87/70"); UiFeature(F_SURFACES); >;
float g_flSnowCover < UiType(Slider); Default(1.0); Range(0.0, 1.0); UiGroup("Surfaces,87/80"); UiFeature(F_SURFACES); >;
float g_flSnowBrightness < UiType(Slider); Default(0.4); Range(0.0, 1.0); UiGroup("Surfaces,87/90"); UiFeature(F_SURFACES); >;

// Impacts. Splash Amount: how many drops splash where they land (with the rain's Amount, showers and gusts). Splash
// Size: how big a splash is. Splash Refraction: how strongly the splash's water bends the scene behind it (each bit a
// tiny lens: what is round it, upside down and spread). Splash Blur: how soft that view is. Splash Brightness: how
// much light the splash catches. Ripple Strength: how much the rings on puddles and wet flat ground bend what they
// reflect. Ripple Size: how wide the rings open.
float g_flSplashAmount < UiType(Slider); Default(1.0); Range(0.0, 4.0); UiGroup("Impacts,86/10"); UiFeature(F_SURFACES); >;
float g_flSplashSize < UiType(Slider); Default(1.0); Range(0.25, 4.0); UiGroup("Impacts,86/20"); UiFeature(F_SURFACES); >;
float g_flSplashRefraction < UiType(Slider); Default(1.5); Range(0.0, 4.0); UiGroup("Impacts,86/23"); UiFeature(F_SURFACES); >;
float g_flSplashBlur < UiType(Slider); Default(0.2); Range(0.0, 1.0); UiGroup("Impacts,86/25"); UiFeature(F_SURFACES); >;
float g_flSplashBrightness < UiType(Slider); Default(1.6); Range(0.0, 4.0); UiGroup("Impacts,86/27"); UiFeature(F_SURFACES); >;
float g_flRippleStrength < UiType(Slider); Default(1.0); Range(0.0, 4.0); UiGroup("Impacts,86/30"); UiFeature(F_SURFACES); >;
float g_flRippleSize < UiType(Slider); Default(1.0); Range(0.25, 4.0); UiGroup("Impacts,86/40"); UiFeature(F_SURFACES); >;

// The scene's surface where the rain meets it.
struct Ground
{
    float3 p;           // the scene point (world)
    float3 n;           // its normal, facing the camera
    float foot;         // under the rain: inside the box's footprint (soft over Edge Fade), 0 elsewhere
    float open;         // open to the sky (SkyOpen)
    float2 heights;     // SkyHeights: x below its surroundings (a hollow above 0), y below the top above it
    float outline;      // at an object's outline (SceneNormal): 1 there, 0 inside a surface
    float corner;       // at an edge or a corner, where the normal flips between faces (SceneNormal): 1 there
};

static Ground s_ground;

// The scene's point along the ray through pixel (dir that ray).
float3 ScenePointAlong(float2 pixel, float3 camera, float3 dir)
{
    return camera + dir * SceneDistance(pixel, camera, dir);
}

// The scene's point k pixels along (dx, dy) from this one: mesh, meshDx, meshDy are the carrier's surface point at this
// pixel and its steps to the next pixel right and down (taken at the top of MainPs, in uniform flow), which give the
// other pixel's view ray.
float3 ScenePointOver(float2 pixel, float3 camera, float3 mesh, float3 meshDx, float3 meshDy, float2 step)
{
    return ScenePointAlong(pixel + step, camera, normalize(mesh + meshDx * step.x + meshDy * step.y - camera));
}

// The scene's normal at p (the pixel's point) from its neighbours' depths (the engine keeps no normals). The depth
// holds a mesh's flat faces, not the smoothed normals the engine shades with, and a wet sheen drawn on them shows every
// face. So where the scene is one smooth surface 10 pixels out on both sides, the slope is taken across that span,
// which smooths over the faces (and over the depth's own precision on distant ground). Where it is not (an edge, a
// step), on each axis three pixels out, from the side whose point lies nearer p: no normal across a depth edge. (The
// side chosen by the next pixel instead let a pixel two from an edge take its slope across it: a light ring round
// everything, the wetness thinned on its tilted normal.) outline: 1 where p is at an object's outline (the depth jumps
// within the span to something well behind p), where the normal turns edge on to the eye. corner: 1 where the three
// pixel sides break too (an edge, a corner), where along the edge's pixel steps the normal flips from face to face.
float3 SceneNormal(float2 pixel, float3 camera, float3 mesh, float3 meshDx, float3 meshDy, float3 p, out float outline,
                   out float corner)
{
    const float3 r = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(3.0, 0.0));
    const float3 l = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(-3.0, 0.0));
    const float3 d = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(0.0, 3.0));
    const float3 u = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(0.0, -3.0));
    const float3 hx = dot(r - p, r - p) < dot(l - p, l - p) ? r - p : p - l;
    const float3 hy = dot(d - p, d - p) < dot(u - p, u - p) ? d - p : p - u;
    const float3 r10 = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(10.0, 0.0));
    const float3 l10 = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(-10.0, 0.0));
    const float3 d10 = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(0.0, 10.0));
    const float3 u10 = ScenePointOver(pixel, camera, mesh, meshDx, meshDy, float2(0.0, -10.0));
    // A span is one surface where its two halves run the same way (a curved surface turns a little, an edge or a
    // corner much) and the depth runs on without a jump. The jump by the inverse distance, which a flat surface keeps
    // in step across the screen: by the turn alone a jump at an outline passes, the surface there runs along the view
    // ray as the jump does (a sphere's top seen from above). Each axis on its own: a span broken on one axis takes
    // that axis's slope from three pixels and keeps the other's from ten (blended, a span crossing a ledge bent the
    // normal along a row next to it: a glint line).
    const float here = length(p - camera);
    const float2 jump3 = abs(here / float2(length(r - camera), length(d - camera)) +
                             here / float2(length(l - camera), length(u - camera)) - 2.0);
    const float2 jump10 = abs(here / float2(length(r10 - camera), length(d10 - camera)) +
                              here / float2(length(l10 - camera), length(u10 - camera)) - 2.0);
    const float2 turn3 = 1.0 - float2(dot(normalize(r - p), normalize(p - l)), dot(normalize(d - p), normalize(p - u)));
    const float2 turn10 = 1.0 - float2(dot(normalize(r10 - p), normalize(p - l10)), dot(normalize(d10 - p), normalize(p - u10)));
    // Broken from a turn of about 14 degrees (whole from 25) or a jump of a twentieth (whole from a tenth).
    const float2 broken3 = max(saturate((turn3 - 0.03) / 0.06), saturate((jump3 - 0.05) / 0.05));
    const float2 broken10 = max(saturate((turn10 - 0.03) / 0.06), saturate((jump10 - 0.05) / 0.05));
    corner = max(broken3.x, broken3.y);
    const float farthest = max(max(length(r10 - camera), length(l10 - camera)), max(length(d10 - camera), length(u10 - camera)));
    outline = saturate((max(jump10.x, jump10.y) - 0.1) / 0.1) * saturate((farthest / here - 1.1) / 0.1);
    const float3 tx = lerp(normalize(r10 - l10), normalize(hx), broken10.x);
    const float3 ty = lerp(normalize(d10 - u10), normalize(hy), broken10.y);
    const float3 n = normalize(cross(tx, ty));
    return dot(n, camera - p) < 0.0 ? -n : n;
}

// How much of a scene point p lies under the rain: inside the box's footprint (soft over Edge Fade at its sides),
// below the clouds' band, above the rain's end.
float GroundFoot(Volume v, float3 p)
{
    const float3 u = mul(v.toObject, p - v.origin) / v.half;
    const float2 side = 1.0 - abs(u.xy);
    const float w = u.z * v.fallSign;
    if (any(side <= 0.0) || w >= v.rainTop || w < -1.0 - v.rainBelow)
        return 0.0;
    return saturate(min(side.x, side.y) / g_flEdgeFade);
}

// The ground at this pixel (scene: the scene's distance along dir); foot 0 where the rain does not reach it, and then
// nothing else is worked out.
Ground GroundAt(Volume v, float2 pixel, float3 camera, float3 dir, float scene, float3 mesh, float3 meshDx, float3 meshDy)
{
    const float3 p = camera + dir * scene;
    float foot = 0.0;
    if (scene < 1e5 && g_flRainOn > 0.5 && g_flShape > 0.5)
        foot = GroundFoot(v, p);
    float3 n = float3(0.0, 0.0, 1.0);
    float open = 1.0;
    float2 heights = 0.0;
    float outline = 0.0, corner = 0.0;
    if (foot > 0.0)
    {
        n = SceneNormal(pixel, camera, mesh, meshDx, meshDy, p, outline, corner);
        open = SkyOpenSoft(s_sky, p, n);
        heights = SkyHeights(s_sky, p, 48.0 * g_flPuddleSize);
    }
    Ground g;
    g.p = p;
    g.n = n;
    g.foot = foot;
    g.open = open;
    g.heights = heights;
    g.outline = outline;
    g.corner = corner;
    return g;
}

// ------------------------------------------------------------------------------------------ impacts
// A splash's point k (0..13 the crown's ring, 14..19 droplets thrown out) at age a (0..1 of its 0.18 s), from its
// centre c on the ground; h its hash, size its scale (a unit is about an inch: the crown opens to some 2.5 units
// across and rises 1.5, the droplets land 4 to 6 units out).
static const int kCrownPoints = 14;
static const int kSplashPoints = 20;

float3 SplashPoint(int k, float a, float3 c, float3 h, float size)
{
    if (k < kCrownPoints)
    {
        // The crown: a thin ring rising and opening, sinking back as it spreads.
        float s, co;
        sincos((k + h.x) * 6.2831853 / kCrownPoints, s, co);
        const float radius = size * (0.5 + 2.0 * sqrt(a));
        return c + float3(co * radius, s * radius, size * 1.5 * sin(3.14159265 * a));
    }
    // Droplets: thrown up and out at seeded angles, falling back under gravity (386 units/s^2).
    const float3 hk = Hash33(h * 17.0 + k);
    float s, co;
    sincos(hk.x * 6.2831853, s, co);
    const float t = a * 0.18;
    const float reach = size * (15.0 + 20.0 * hk.y) * t;
    return c + float3(co * reach, s * reach, max(size * (25.0 + 20.0 * hk.z) * t - 193.0 * t * t, 0.0));
}

static const float kSplashReach = 1500.0;   // past this, no splashes: the ripples and the wet shine carry the rain

// The splashes where the view ray comes down to the ground (s_ground: the pixel's scene point and normal): rain and
// sleet (sleet's smaller), on open ground facing up. A world-fixed grid of cells, in each a drop landing at a seeded
// moment of every 0.3 s slot (as many as Splash Amount and the rain's Amount make, where the clouds, showers and gusts
// allow). The cells the ray crosses within a splash's height above the ground are walked, the scene point's own
// first, at most six; the ground at a cell is taken as the plane of the scene point, and a splash is kept only where
// the scene's depth at its own place on the screen agrees with it (none floating past an edge). Its points go into r
// with the falling drops and take their light, each a tiny lens (Splash Refraction, Blur, Brightness).
void SplashWalk(inout PrecipResult r, float3 camera, float3 dir, float scene, float2 pixel, float time, float pixelAngle)
{
    const Ground g = s_ground;
    const int kind = PrecipKindNow();
    if (g.foot <= 0.0 || g.open <= 0.0 || (kind != 1 && kind != 3) || g_flSplashAmount <= 0.0 || g.n.z < 0.6 ||
        scene > kSplashReach)
        return;
    const float size = g_flSplashSize * (kind == 3 ? 0.7 : 1.0);
    // Cells some 40 cm wide: at both Amounts 1 about fifteen splashes a square metre a second.
    const float cell = 16.0 * size;
    const float tall = 3.0 * size;
    const float fade = 1.0 - smoothstep(kSplashReach * 0.5, kSplashReach, scene);
    const float chance = saturate(0.8 * g_flSplashAmount * g_flRainAmount);
    // The ray's stretch within tall above the ground, back from the scene point, as world xy in cells.
    const float back = min(tall / max(-dir.z, 0.05), cell * 6.0 / max(length(dir.xy), 0.05));
    const float2 a = g.p.xy / cell;
    const float2 b = (g.p - dir * back).xy / cell;
    const float2 delta = b - a;
    const float2 stepDir = float2(delta.x >= 0.0 ? 1.0 : -1.0, delta.y >= 0.0 ? 1.0 : -1.0);
    const float2 inv = 1.0 / max(abs(delta), 1e-5);
    float2 cur = floor(a);
    const float2 last = floor(b);
    float2 next = float2(stepDir.x > 0.0 ? cur.x + 1.0 - a.x : a.x - cur.x,
                         stepDir.y > 0.0 ? cur.y + 1.0 - a.y : a.y - cur.y) * inv;
    [loop] for (int n = 0; n < 6; n++)
    {
        const float3 h = Hash33(float3(cur, 53.1));
        const float slotTime = time / 0.3 + h.z;
        const float3 hs = Hash33(float3(cur, floor(slotTime)));
        // 0.1 s at most into its slot, a life of 0.18 s: a splash never runs into the next slot.
        const float age = (frac(slotTime) * 0.3 - hs.z * 0.1) / 0.18;
        if (hs.x < chance && age >= 0.0 && age <= 1.0)
        {
            const float2 cxy = (cur + 0.25 + hs.xy * 0.5) * cell;
            const float3 c = float3(cxy, g.p.z - dot(g.n.xy, cxy - g.p.xy) / g.n.z);
            // On a real surface: the scene's depth at the splash's own pixel agrees with it.
            const float4 clip = mul(g_matWorldToProjectionMultiview[0], float4(c, 1.0) + g_vWorldToCameraOffsetMultiview[0]);
            const float2 at = (clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5) * g_vViewportSize + g_vViewportOffset;
            const float cd = length(c - camera);
            if (clip.w > 0.0 && abs(SceneDistance(at, camera, (c - camera) / cd) - cd) < max(2.0, cd * 0.01) &&
                PrecipCover(c + float3(0.0, 0.0, 4.0)) > 0.0)
            {
                [loop] for (int k = 0; k < kSplashPoints; k++)
                {
                    const float3 q = SplashPoint(k, age, c, hs, size);
                    const float th = dot(q - camera, dir);
                    if (th <= 0.0 || th >= scene)
                        continue;
                    const float dist = length(camera + dir * th - q);
                    const float radius = size * (k < kCrownPoints ? 0.35 : 0.3) * (1.0 - 0.5 * age);
                    const float reff = max(radius, th * pixelAngle * 0.6);
                    if (dist >= reff)
                        continue;
                    const float edge = dist / reff;
                    const float alpha = (1.0 - smoothstep(0.4, 1.0, edge)) * (radius / reff) * fade * (1.0 - age * age) *
                                        smoothstep(6.0, 36.0, th);
                    // A tiny lens: the scene round the point seen through it upside down (mirrored through its middle)
                    // and spread, plus a pull of its own (a seeded direction, a few of its widths), softened by Blur.
                    const float4 qc = mul(g_matWorldToProjectionMultiview[0], float4(q, 1.0) + g_vWorldToCameraOffsetMultiview[0]);
                    const float2 qp = (qc.xy / max(qc.w, 1e-4) * float2(0.5, -0.5) + 0.5) * g_vViewportSize + g_vViewportOffset;
                    const float widthPx = reff / max(th * pixelAngle, 1e-6);
                    const float2 pull = Hash33(hs * 7.31 + k).xy - 0.5;
                    const float2 bend = ((qp - pixel) * 4.0 + pull * max(widthPx, 1.0) * 8.0) * g_flSplashRefraction;
                    const float3 behind = SceneBehind(pixel + bend, 1.0 + g_flSplashBlur * 10.0) * g_flSplashBrightness;
                    // The light it sends on, with the glint of its middle (Glints).
                    const float sends = 0.3 * (1.0 + g_flGlints * 2.0 * pow(saturate(1.0 - edge), 4.0));
                    if (r.hits == 0.0)
                        r.litAt = q;
                    r.color += r.transmittance * behind * alpha;
                    r.lit += r.transmittance * sends * alpha;
                    r.transmittance *= 1.0 - alpha;
                    r.hits += 1.0;
                }
            }
        }
        if (all(cur == last))
            break;
        if (next.x < next.y)
        {
            cur.x += stepDir.x;
            next.x += inv.x;
        }
        else
        {
            cur.y += stepDir.y;
            next.y += inv.y;
        }
    }
}

// The ripples on a flat wet surface at p: rings from drops landing at seeded moments in world-fixed cells (3 x 3 round
// p), each opening over 0.6 s and fading. The slope (world xy) to tilt the normal by.
float2 RippleSlope(float3 p, float time)
{
    const float cell = 10.0 * g_flRippleSize;
    const float2 q = p.xy / cell;
    const float2 base = floor(q);
    const float chance = saturate(0.8 * g_flRainAmount);
    float2 slope = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
    {
        [unroll] for (int x = -1; x <= 1; x++)
        {
            const float2 c = base + float2(x, y);
            const float local = time / 0.9 + Hash33(float3(c, 91.7)).z;
            const float3 h = Hash33(float3(c, floor(local)));
            const float age = frac(local) / 0.67;
            if (h.x >= chance || age > 1.0)
                continue;
            const float2 d = q - (c + 0.2 + h.yz * 0.6);
            const float dist = length(d);
            const float s = (dist - age * 1.1) * 14.0;
            slope += d / max(dist, 1e-4) * cos(s * 2.0) * exp(-s * s * 0.5) * (1.0 - age) * (1.0 - age);
        }
    }
    // A raindrop's ring tilts the water some 8 degrees at its crest (a steeper tilt turned puddle reflections back
    // onto what stands just in front of them).
    return slope * 0.14 * g_flRippleStrength;
}

// ------------------------------------------------------------------------------------------ accumulation
// The light reaching a ground point p with normal n seen from v (the host's, weather.vfx): the diffuse return of the
// sun and every S2FM light (Valve's scale: no 1/pi), and in specular their highlight on a water film of the given
// roughness. One call a pixel: it holds the light loops.
float3 GroundLight(float3 p, float3 n, float3 v, float roughness, out float3 specular);

// A world point on the screen: xy in -1..1 (y up), z the clip w (0 or less: behind the camera).
float3 GroundNdc(float3 p)
{
    const float4 clip = mul(g_matWorldToProjectionMultiview[0], float4(p, 1.0) + g_vWorldToCameraOffsetMultiview[0]);
    return float3(clip.xy / max(clip.w, 1e-4), clip.w);
}

// The reflection of the ray r from p in what is on the screen (the method of s2vfx_water's): marched through the
// scene's depth in 24 steps growing with distance out to 3000 units (the first jittered per pixel), the crossing
// found by halving, the point read from the frame copy blurred by the roughness. How much it counts: 0 for a miss,
// fading at the screen's edge and for rays turned toward the camera.
float GroundReflection(float3 p, float3 r, float3 v, float2 pixel, float3 camera, float roughness, out float3 color)
{
    color = 0.0;
    const float toward = saturate(1.0 - dot(r, v) * 4.0);
    if (toward <= 0.0)
        return 0.0;
    const float3 origin = GroundNdc(p);
    const float2 toPixels = float2(0.5, -0.5) * g_vViewportSize;
    const float grow = 1.12;
    float stepLength = 3000.0 * (grow - 1.0) / (pow(grow, 24.0) - 1.0);
    float t = stepLength * frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    [loop] for (int k = 0; k < 24; k++)
    {
        t += stepLength;
        const float3 q = p + r * t;
        const float3 ndc = GroundNdc(q);
        if (ndc.z <= 0.0 || any(abs(ndc.xy) > 1.0))
            return 0.0;
        const float rayDistance = length(q - camera);
        const float sceneDistance = SceneDistance(pixel + (ndc.xy - origin.xy) * toPixels, camera, (q - camera) / rayDistance);
        if (rayDistance > sceneDistance)
        {
            // Behind something thicker than 20 units the ray goes on: it may come out behind it. (More caught rays
            // passing just behind an object's back, its copy along it.)
            if (rayDistance - sceneDistance > 20.0 + stepLength)
            {
                stepLength *= grow;
                continue;
            }
            float a = t - stepLength, b = t;
            [unroll] for (int h = 0; h < 4; h++)
            {
                const float m = (a + b) * 0.5;
                const float3 mq = p + r * m;
                const float md = length(mq - camera);
                const float3 mn = GroundNdc(mq);
                if (md > SceneDistance(pixel + (mn.xy - origin.xy) * toPixels, camera, (mq - camera) / md))
                    b = m;
                else
                    a = m;
            }
            const float3 hit = GroundNdc(p + r * b);
            const float2 edge = saturate((1.0 - abs(hit.xy)) / 0.1);
            const float radius = roughness * roughness * b / max(length(p + r * b - camera), 1.0) * g_vViewportSize.y * 0.5;
            color = SceneBehind(pixel + (hit.xy - origin.xy) * toPixels, radius);
            return edge.x * edge.y * toward;
        }
        stepLength *= grow;
    }
    return 0.0;
}

// The shape noise's value below which a share s (0..1) of it lies: its deciles, measured over the atlas
// tests/weather_noise.py writes. Puddles turn a share of the ground into the water's level with it.
float NoiseLevel(float s)
{
    static const float kDeciles[11] = { 0.0, 0.286, 0.388, 0.459, 0.518, 0.573, 0.62, 0.675, 0.733, 0.808, 1.0 };
    const float at = saturate(s) * 10.0;
    const int k = min((int)at, 9);
    return lerp(kDeciles[k], kDeciles[k + 1], at - k);
}

// How much of the ground at this pixel the precipitation has made wet, a puddle and snow (x, y, z, 0..1), over time.
// By kind: rain wets; sleet wets and lays a little slush; hail wets and scatters white; snow covers. Wetness is a
// little less on steep faces (the water runs off; wind still drives the rain at them). Puddles: on flat ground, a dryness from a world-fixed noise less the sky map's
// hollows, flooded from the driest last as the level rises to cover a share Puddles of it over time; none on what
// stands over all round it (a dome's top, a lid, a step: the water runs off). Snow: on the
// tops (nothing above it in the sky map), by slope, its edge broken by noise.
float3 GroundMasks(Ground g, float time)
{
    const int kind = PrecipKindNow();
    const float ramp = g_flSurfaceFill > 0.0 ? saturate((time - g_flSurfaceStart) / g_flSurfaceFill) : 1.0;
    const float open = g.open * g.foot;
    const float up = saturate(g.n.z);
    const float wetKind = kind == 2 ? 0.0 : (kind == 3 ? 0.8 : 1.0);
    const float snowKind = kind == 2 ? 1.0 : (kind == 3 ? 0.35 : (kind == 4 ? 0.2 : 0.0));
    const float wet = g_flWetness * wetKind * open * ramp * lerp(0.7, 1.0, up);
    float puddle = 0.0;
    if (wet > 0.0 && g_flPuddles > 0.0 && up > 0.9)
    {
        const float noise = ShapeNoise(float3(g.p.xy / (600.0 * g_flPuddleSize), 0.43)).r;
        const float dryness = noise - saturate(g.heights.x / (6.0 * g_flPuddleSize)) * 0.3;
        puddle = saturate((NoiseLevel(g_flPuddles * ramp) - dryness) / 0.03) * smoothstep(0.9, 0.97, up) * open *
                 saturate(1.0 + g.heights.x / 2.0);
    }
    float snow = 0.0;
    if (snowKind > 0.0 && g_flSnowCover > 0.0)
    {
        const float top = 1.0 - smoothstep(4.0, 12.0, g.heights.y);
        const float breakup = DetailNoise(float3(g.p.xy / 90.0, g.p.z / 90.0)).r;
        snow = saturate(g_flSnowCover * snowKind * open * ramp * smoothstep(0.35, 0.8, up) * top * 1.5 - breakup * 0.5);
    }
    return float3(wet, puddle, snow);
}

// The sky the wet ground reflects where the march finds nothing: the frame where the far end of r falls (moved onto
// the screen, to its edge toward r), blurred, if the scene there is the sky; the S2FM ambient otherwise. Not the
// probe's ambient: that is the carrier's, high in the sky, and measured on this ground it was its volume's edge.
float3 GroundSky(float3 p, float3 r, float2 pixel, float3 camera)
{
    const float3 far = GroundNdc(p + r * 1e5);
    if (far.z > 0.0)
    {
        const float2 at = pixel + (clamp(far.xy, -0.98, 0.98) - GroundNdc(p).xy) * float2(0.5, -0.5) * g_vViewportSize;
        if (SceneDistance(at, camera, r) >= 1e5)
            return SceneBehind(at, 16.0);
    }
    return g_vToolsAmbientLightingVr.xyz;
}

// What the precipitation has made of the ground at this pixel: returns the multiplier on what is behind (darkening,
// covering); add gets the light added on top: the wet film's and the puddles' reflections (screen space, the sky
// where the ray finds nothing) bent by the ripples, the lights' highlights on the water, the snow lit white.
float GroundShade(Ground g, float3 camera, float2 pixel, float time, out float3 add)
{
    add = 0.0;
    if (g.foot <= 0.0 || PrecipKindNow() <= 0)
        return 1.0;
    const float3 masks = GroundMasks(g, time);
    const float wet = masks.x, puddle = masks.y, snow = masks.z;
    const float film = max(wet, puddle);
    if (film <= 0.0 && snow <= 0.0)
        return 1.0;
    const float3 v = normalize(camera - g.p);
    const float3 n = film > 0.0 ? normalize(g.n + float3(RippleSlope(g.p, time) * max(puddle, wet * 0.3), 0.0)) : g.n;
    // Off level ground no sharper than 0.3: there the normal is the depth's guess, not the model's, and a sharp
    // highlight drew a one pixel glint line where a bevel's turn met the light's angle.
    const float level = smoothstep(0.9, 0.97, g.n.z);
    const float sharp = lerp(lerp(0.35, 0.08, wet), 0.02, puddle);
    const float roughness = lerp(max(sharp, 0.3), sharp, level);
    float3 specular;
    const float3 lit = GroundLight(g.p, n, v, roughness, specular);
    float mul = 1.0;
    if (film > 0.0)
    {
        const float3 r = reflect(-v, n);
        float3 seen;
        const float hit = GroundReflection(g.p, r, v, pixel, camera, roughness, seen);
        const float3 reflected = lerp(GroundSky(g.p, r, pixel, camera), seen, hit);
        const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(n, v)), 5.0);
        // Not at an object's outline: there the normal from the depth turns edge on (or is a flat face's), and the
        // water's Fresnel, highlight and the march's jitter made a bright, noisy line round it. Ground seen at a
        // grazing angle is no outline and keeps its reflection.
        const float outline = 1.0 - g.outline;
        mul *= (1.0 - g_flDarkening * wet) * (1.0 - 0.3 * puddle) * (1.0 - fresnel * puddle);
        // Not where the normal flips along an edge or a ledge either (a dotted line of glints, a bright line): the
        // highlight nowhere there, the reflection only on level ground (where a puddle's reflection meets what
        // stands in it).
        add += (reflected * fresnel * g_flWetReflection * (1.0 - g.corner * (1.0 - level)) + specular * (1.0 - g.corner)) *
               film * outline;
    }
    if (snow > 0.0)
    {
        // Snow Brightness (sleet's slush two thirds of it) under the brighter of the light found here and the light the
        // engine put on the ground: the scene's colour over a typical ground's albedo (0.25) carries the map's baked
        // light, which the lights here leave out.
        const float3 engine = dot(SceneBehind(pixel, 4.0), float3(0.2126, 0.7152, 0.0722)) / 0.25;
        const float3 color = g_flSnowBrightness * (PrecipKindNow() == 3 ? 0.65 : 1.0) * max(lit + g_vToolsAmbientLightingVr.xyz, engine);
        mul *= 1.0 - snow;
        add = add * (1.0 - snow) + color * snow;
    }
    return mul;
}
