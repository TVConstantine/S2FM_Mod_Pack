// Precipitation for s2vfx_weather, written from scratch for this project: rain, snow, sleet and hail falling from under
// a layer's clouds. Near the camera it is particles: the air is cut into cells fixed in the falling air, each holding
// at most one particle whose place, size and flutter come from a hash of the cell (nothing is stored), and the view ray
// walks those cells at three scales. Farther it is the curtain the host marches; the helpers at the end give that
// curtain its showers, gusts, virga, texture, look and rainbow.
// A PS block includes this after material.hlsl, screen.hlsl and weather_noise.hlsl, and defines after it (the two are
// declared here):
//   float PrecipCover(float3 world)      0..1, the share of particles kept at a world point: the clouds above it, the
//                                        showers, gusts and virga there; 0 outside the rain
//   float3 PrecipLight(float3 world, float3 dir, float g)
//                                        the light a particle at world sends on along dir (from the eye): every light
//                                        through a Henyey-Greenstein lobe of asymmetry g, scaled so an even lobe gives
//                                        the light itself, and the sky
// It reads g_flRainOn, the kind (F_RAIN: 0 none, 1 rain, 2 snow, 3 sleet, 4 hail).

float PrecipCover(float3 world);
float3 PrecipLight(float3 world, float3 dir, float g);

// Amount: how much falls (the share of cells holding a particle, and the curtain's thickness). Drop Size: the
// particles' size. Streak Length: how long a drop is drawn, as the way it falls while a shutter is open (0 a round
// drop, for S2FM's motion blur to stretch; 0.5 a 1/48 s shutter; 1 twice that). Fall Speed: how fast it falls (each kind at its own pace). Wind Lean: the
// wind across the fall (snow, falling slowly, leans the most). Gustiness: waves of thicker and thinner precipitation
// running along the wind. Turbulence: how much snow and sleet flutter. Variation: showers, the curtain thick in places
// and thin in others. Evaporation: virga, the curtain thinning as it falls and at high values never reaching the
// ground. Sleet Mix: snow's share in sleet. Glints: the sparkle of drops and flakes catching a light. Rainbow: a bow in
// the rain with the sun behind the camera. Tint, Darkness: the particles' colour, and how grey they are against the
// light. Refraction: how much drops bend the scene behind them. The curtain has a group of its own (Haze, below).
float g_flRainAmount < UiType(Slider); Default(0.5); Range(0.0, 2.0); UiGroup("Precipitation,80/10"); UiFeature(F_RAIN); >;
float g_flDropSize < UiType(Slider); Default(1.0); Range(0.2, 4.0); UiGroup("Precipitation,80/20"); UiFeature(F_RAIN); >;
float g_flStreakLength < UiType(Slider); Default(0.6); Range(0.0, 1.0); UiGroup("Precipitation,80/30"); UiFeature(F_RAIN); >;
float g_flRainSpeed < UiType(Slider); Default(1.0); Range(0.0, 8.0); UiGroup("Precipitation,80/40"); UiFeature(F_RAIN); >;
float g_flWindLean < UiType(Slider); Default(0.15); Range(0.0, 2.0); UiGroup("Precipitation,80/50"); UiFeature(F_RAIN); >;
float g_flGustiness < UiType(Slider); Default(0.3); Range(0.0, 1.0); UiGroup("Precipitation,80/60"); UiFeature(F_RAIN); >;
float g_flTurbulence < UiType(Slider); Default(0.5); Range(0.0, 1.0); UiGroup("Precipitation,80/70"); UiFeature(F_RAIN); >;
float g_flVariation < UiType(Slider); Default(0.4); Range(0.0, 1.0); UiGroup("Precipitation,80/80"); UiFeature(F_RAIN); >;
float g_flEvaporation < UiType(Slider); Default(0.0); Range(0.0, 1.0); UiGroup("Precipitation,80/90"); UiFeature(F_RAIN); >;
float g_flSleetMix < UiType(Slider); Default(0.5); Range(0.0, 1.0); UiGroup("Precipitation,80/100"); UiFeature(F_RAIN); >;
float g_flGlints < UiType(Slider); Default(0.5); Range(0.0, 2.0); UiGroup("Precipitation,80/110"); UiFeature(F_RAIN); >;
float g_flRainbow < UiType(Slider); Default(0.0); Range(0.0, 2.0); UiGroup("Precipitation,80/120"); UiFeature(F_RAIN); >;
float3 g_vRainTint < UiType(Color); Default3(1.0, 1.0, 1.0); UiGroup("Precipitation,80/130"); UiFeature(F_RAIN); >;
float g_flRainDarkness < UiType(Slider); Default(0.5); Range(0.0, 1.0); UiGroup("Precipitation,80/140"); UiFeature(F_RAIN); >;
float g_flRainRefraction < UiType(Slider); Default(0.5); Range(0.0, 1.0); UiGroup("Precipitation,80/150"); UiFeature(F_RAIN); >;
// The haze: the curtain, the precipitation too far or too fine to see one by one. Density: how thick it is (at 1 and
// Amount 1 about 3000 units of it hide what is behind; 0 none, the particles alone). Brightness: how much of the light
// reaching it it sends on. Glow: how much brighter it is round a light behind it (0 the same seen from any side, 1 its
// kind's own forward glow). Color: its tint. Refraction: how much it bends and blurs the scene behind it. Near: how
// close to the camera it comes (about half there at Near, thinner closer, the particles alone at the lens). Far: where
// it ends (thinning from 0.6 of it).
float g_flHazeDensity < UiType(Slider); Default(1.0); Range(0.0, 4.0); UiGroup("Haze,85/10"); UiFeature(F_RAIN); >;
float g_flHazeBrightness < UiType(Slider); Default(0.5); Range(0.0, 4.0); UiGroup("Haze,85/20"); UiFeature(F_RAIN); >;
float g_flHazeGlow < UiType(Slider); Default(0.7); Range(0.0, 1.0); UiGroup("Haze,85/30"); UiFeature(F_RAIN); >;
float3 g_vHazeColor < UiType(Color); Default3(1.0, 1.0, 1.0); UiGroup("Haze,85/40"); UiFeature(F_RAIN); >;
float g_flHazeRefraction < UiType(Slider); Default(0.2); Range(0.0, 1.0); UiGroup("Haze,85/50"); UiFeature(F_RAIN); >;
float g_flHazeNear < UiType(Slider); Default(256.0); Range(0.0, 10000.0); UiGroup("Haze,85/60"); UiFeature(F_RAIN); >;
float g_flHazeFar < UiType(Slider); Default(100000.0); Range(100.0, 200000.0); UiGroup("Haze,85/70"); UiFeature(F_RAIN); >;
// Precipitation Quality: the cells walked at each of the three scales (more: particles farther out, slower).
float g_flPrecipQuality < UiType(Slider); Default(16.0); Range(4.0, 48.0); UiGroup("Quality,90/30"); UiFeature(F_RAIN); >;

int PrecipKindNow()
{
    return (int)(g_flRainOn + 0.5);
}

// A particle's look: 0 a rain drop, 1 a snow flake, 2 a hail stone, 3 a wet snow clump (sleet's snow).
struct PrecipLook
{
    float speed;        // fall speed, world units a second, before Fall Speed
    float radius;       // world units, before Drop Size
    float tall;         // a cell's height over its width (a streak fits inside)
    float wander;       // how far it flutters in its cell, a share of the cell, before Turbulence
    float streak;       // how much of Streak Length it draws (a drop 1, a flake 0)
    float density;      // the share of cells holding one at Amount 1
};

PrecipLook LookOf(int look)
{
    PrecipLook l;
    l.speed = 360.0; l.radius = 0.06; l.tall = 4.0; l.wander = 0.0; l.streak = 1.0; l.density = 1.0;
    if (look == 1)
    {
        l.speed = 45.0; l.radius = 0.25; l.tall = 1.0; l.wander = 0.22; l.streak = 0.0; l.density = 0.6;
    }
    else if (look == 2)
    {
        l.speed = 700.0; l.radius = 0.18; l.tall = 3.0; l.wander = 0.0; l.streak = 0.4; l.density = 0.25;
    }
    else if (look == 3)
    {
        l.speed = 160.0; l.radius = 0.22; l.tall = 2.0; l.wander = 0.08; l.streak = 0.15; l.density = 0.5;
    }
    return l;
}

// The kind's main look (sleet's drops) and its fall speed, world units a second.
int PrecipMainLook()
{
    const int kind = PrecipKindNow();
    return kind == 2 ? 1 : (kind == 4 ? 2 : 0);
}

float PrecipFallSpeed()
{
    return LookOf(PrecipMainLook()).speed;
}

// The three scales: cells 16, 64 and 256 units wide, each walked Precipitation Quality cells out (along a level view).
float PrecipCell(int level)
{
    return 16.0 * pow(4.0, (float)level);
}

float PrecipCells()
{
    return clamp(g_flPrecipQuality, 4.0, 48.0);
}

// Where the particles end (the far scale's reach).
float PrecipReach()
{
    return PrecipCell(2) * PrecipCells();
}

// The curtain's share at a distance along the view: thin just in front of the camera, where only the near particles
// are seen, about half at Haze Near and whole a little past it (the particles then add their streaks over it), gone
// at Haze Far.
float PrecipHazeFade(float t)
{
    const float near = g_flHazeNear > 0.0 ? smoothstep(g_flHazeNear * 0.25, g_flHazeNear * 1.75, t) : 1.0;
    return near * (1.0 - smoothstep(g_flHazeFar * 0.6, g_flHazeFar, t));
}

// A scale's share at a distance along the view: in from where the scale before it fades, out at its own reach (the
// last one out more slowly, into the curtain).
float PrecipFade(int level, float t)
{
    const float end = PrecipCell(level) * PrecipCells();
    float f = 1.0 - smoothstep(end * (level == 2 ? 0.5 : 0.75), end, t);
    if (level > 0)
    {
        const float before = PrecipCell(level - 1) * PrecipCells();
        f *= smoothstep(before * 0.75, before, t);
    }
    return f;
}

// What the particles a ray meets give: their light (weighted by what is in front of each), how much of what is behind
// them shows, and for the debug views how many were hit and the scale of the nearest. While walking, the light they
// send on is gathered as a weight (lit) and the place of the nearest (litAt): the lights are weighed once, there, in
// PrecipParticles (the nearest dominates; and one light call keeps the shader small).
struct PrecipResult
{
    float3 color;
    float transmittance;
    float hits;
    float level;
    float3 lit;
    float3 litAt;
};

#if S2VFX_PRECIP_SPLASHES
// The splashes where the precipitation lands (s2vfx/weather_ground.hlsl), walked with the particles: they come into r
// the same way and take the same light.
void SplashWalk(inout PrecipResult r, float3 camera, float3 dir, float scene, float2 pixel, float time, float pixelAngle);
#endif

// A particle's look toward the eye: what it shows of the scene behind (behind, already lit) and how much of the light
// reaching it it sends on (the return). edge: how far off the particle's middle the ray passes (0 middle, 1 edge); h
// its hash, for its own glint and bend.
float PrecipShade(int look, float2 pixel, float3 h, float edge, float time, out float3 behind)
{
    behind = 0.0;
    if (look == 1 || look == 3)
    {
        // Snow: soft and nearly white, its light mostly sent back; now and then a flake turns into the light.
        const float turn = frac(h.y * 7.0 + time * (0.3 + h.z));
        const float sparkle = 1.0 + g_flGlints * 4.0 * step(0.96, h.z) * pow(saturate(1.0 - abs(turn - 0.5) * 8.0), 2.0);
        return (look == 1 ? 0.85 : 0.6) * sparkle * (1.0 - g_flRainDarkness * 0.5);
    }
    // Water and ice: a drop is a tiny lens, gathering a wide, blurred view of what is round it, brighter than the scene
    // just behind it (Darkness dims it: grey rain), plus the light it sends on (strongly forward) and a glint in its
    // middle. So drops read against any background, as rain does.
    const float2 bend = (h.xy - 0.5) * (1.0 - edge) * 10.0 * g_flRainRefraction;
    behind = SceneBehind(pixel + bend, 6.0) * lerp(1.8, 0.8, g_flRainDarkness);
    return (look == 2 ? 0.5 : 0.3) * (1.0 + g_flGlints * (look == 2 ? 3.0 : 2.0) * pow(saturate(1.0 - edge), 4.0));
}

// One kind of particle at one scale: the view ray (camera, dir) walked from range.x to range.y through the cells of a
// grid that falls with velocity vel (world units a second); share: the kind's part (sleet splits its cells).
void PrecipWalk(inout PrecipResult r, int look, float share, int level, float3 camera, float3 dir, float2 range,
                float scene, float2 pixel, float3 wind, float time, float pixelAngle)
{
    const PrecipLook l = LookOf(look);
    const float w = PrecipCell(level);
    const float3 size = float3(w, w, w * l.tall);
    const float speed = l.speed * g_flRainSpeed;
    const float3 vel = float3(wind.xy * min(g_flWindLean * 360.0, l.speed * 3.0) * g_flRainSpeed, -speed);
    const float3 fall = speed > 0.0 ? normalize(vel) : float3(0.0, 0.0, -1.0);
    const float baseRadius = l.radius * g_flDropSize;
    // The streak: the way it falls in a shutter (Streak Length 0.5: 1/48 s), at most what fits in its cell.
    const float shutterLen = min(l.streak * speed * g_flStreakLength * (2.0 / 48.0), size.z * 0.7);
    const float wander = l.wander * g_flTurbulence;
    const float keep = saturate(l.density * share * g_flRainAmount);
    // Coverage grows with the scale, so a farther scale's sparser particles keep about the same look.
    const float weight = pow(4.0, (float)level);

    // The ray in the falling grid's cells: a 3D DDA (each step to the nearest cell wall).
    const float3 shift = vel * time;
    const float3 o = (camera - shift) / size;
    const float3 d = dir / size;
    const float3 s = float3(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    const float3 inv = 1.0 / max(abs(d), 1e-7);
    float t = range.x;
    const float3 p = o + d * t;
    float3 cell = floor(p);
    float3 next = t + float3(s.x > 0.0 ? cell.x + 1.0 - p.x : p.x - cell.x, s.y > 0.0 ? cell.y + 1.0 - p.y : p.y - cell.y,
                             s.z > 0.0 ? cell.z + 1.0 - p.z : p.z - cell.z) * inv;
    const int cells = (int)PrecipCells() * 2;
    [loop] for (int n = 0; n < cells; n++)
    {
        if (t >= range.y || r.transmittance < 0.02)
            break;
        const float3 h = Hash33(cell + float3(level * 41.0 + look * 13.0, 7.1, 3.3));
        if (h.x < keep)
        {
            // Its size (bigger drops fall faster, so they streak longer), and its place, away from the walls by its
            // flutter, its size and half its streak.
            const float3 h2 = Hash33(cell * 1.731 + float3(19.2, 4.4, level * 5.0 + look));
            const float grow = 0.55 + frac(h.y * 13.7 + h.z * 7.1) * 0.9;
            const float radius = baseRadius * grow;
            const float len = shutterLen * grow;
            const float3 margin = min(float3(wander + radius / size.x, wander + radius / size.y,
                                             wander + (radius + len * 0.5) / size.z), 0.49);
            float3 local = margin + h2 * (1.0 - 2.0 * margin);
            if (wander > 0.0)
            {
                const float3 beat = (0.6 + h * 0.9) * 6.2831853;
                local += wander * float3(sin(time * beat.x + h2.y * 6.2831853), cos(time * beat.y + h2.z * 6.2831853),
                                         0.5 * sin(time * beat.z + h2.x * 6.2831853));
            }
            const float3 centre = (cell + local) * size + shift;
            // The view ray's closest approach to the particle's streak (a segment along its fall).
            const float3 a0 = centre - fall * (len * 0.5);
            const float3 w0 = camera - a0;
            const float b = dot(dir, fall);
            const float along = (dot(fall, w0) - b * dot(dir, w0)) / max(1.0 - b * b, 1e-6);
            const float u = clamp(along, 0.0, len);
            const float3 q = a0 + fall * u;
            const float th = dot(q - camera, dir);
            const float dist = length(camera + dir * th - q);
            // Thinner than a pixel: drawn a pixel wide, its coverage thinned instead (no shimmer).
            const float reff = max(radius, max(th, 1.0) * pixelAngle * 0.6);
            if (th > range.x && th < range.y && th < scene && dist < reff)
            {
                // Kept where the clouds, showers and gusts want it, fading in and out as those change (no pop).
                const float present = keep * PrecipCover(centre);
                const float edge = dist / reff;
                const float thin = radius / reff;
                float a = (1.0 - smoothstep(0.4, 1.0, edge)) * (look == 1 || look == 3 ? thin * thin * 0.9 : thin);
                // A streak spends only part of the shutter at a point, and fades toward its rounded ends.
                a *= len > 0.0 ? sqrt(sin(3.14159265 * saturate((along + radius) / (len + 2.0 * radius)))) *
                                 sqrt(2.0 * radius / (len + 2.0 * radius) * 0.5 + 0.5) : 1.0;
                a *= saturate((present - h.x) / max(present * 0.3, 1e-4));
                // Close to the lens it would be a blur, not a drop.
                a *= smoothstep(6.0, 36.0, th);
                a = saturate(a * weight * PrecipFade(level, th));
                float3 behind;
                const float sends = PrecipShade(look, pixel, h, edge, time, behind);
                if (r.hits == 0.0)
                    r.litAt = centre;
                r.color += r.transmittance * behind * a;
                r.lit += r.transmittance * sends * a;
                r.transmittance *= 1.0 - a;
                r.hits += 1.0;
                if (r.level < 0.0)
                    r.level = (float)level;
            }
        }
        // On to the next cell.
        const float tn = min(next.x, min(next.y, next.z));
        const float3 m = next.x <= tn ? float3(1.0, 0.0, 0.0) : (next.y <= tn ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0));
        cell += m * s;
        next += m * inv;
        t = tn;
    }
}

// The particles along the view ray between range.x and range.y (the rain's stretch), cut at the scene (scene: its
// distance). wind: the wind's direction in the world (z 0).
PrecipResult PrecipParticles(float3 camera, float3 dir, float2 range, float scene, float2 pixel, float3 wind, float time)
{
    PrecipResult r;
    r.color = 0.0;
    r.transmittance = 1.0;
    r.hits = 0.0;
    r.level = -1.0;
    r.lit = 0.0;
    r.litAt = 0.0;
    const int kind = PrecipKindNow();
    if (kind <= 0 || g_flRainAmount <= 0.0)
        return r;
    const float pixelAngle = 2.0 / max(abs(g_matViewToProjection[1][1]) * g_vViewportSize.y, 1e-3);
    const int look = PrecipMainLook();
    // Scale by scale, near first; sleet walks each scale twice, its drops and then its wet snow clumps. None when the
    // ray has no stretch of falling precipitation (the splashes below may still be there).
    const int passes = range.x >= range.y ? 0 : (kind == 3 ? 6 : 3);
    [loop] for (int walk = 0; walk < passes; walk++)
    {
        const int level = kind == 3 ? walk / 2 : walk;
        const bool clump = kind == 3 && (walk & 1) != 0;
        const float end = PrecipCell(level) * PrecipCells();
        const float start = level == 0 ? 0.0 : PrecipCell(level - 1) * PrecipCells() * 0.75;
        const float2 span = float2(max(range.x, start), min(range.y, end));
        if (span.x >= span.y)
            continue;
        const float share = kind != 3 ? 1.0 : (clump ? g_flSleetMix : 1.0 - g_flSleetMix);
        PrecipWalk(r, clump ? 3 : look, share, level, camera, dir, span, scene, pixel, wind, time, pixelAngle);
    }
#if S2VFX_PRECIP_SPLASHES
    // The splashes on the ground, behind the falling particles.
    SplashWalk(r, camera, dir, scene, pixel, time, pixelAngle);
#endif
    // The light they send on, weighed at the nearest: forward for drops, evenly for snow.
    if (r.hits > 0.0)
    {
        const float lobe = kind == 2 ? 0.25 : (kind == 3 ? 0.5 : (kind == 4 ? 0.6 : 0.8));
        r.color += r.lit * PrecipLight(r.litAt, dir, lobe);
    }
    r.color *= SrgbGammaToLinear(g_vRainTint);
    return r;
}

// ------------------------------------------------------------------------------------------ the curtain
// Showers and gusts at q (cloud widths, the clouds' noise space), wind its direction there: thick in places and thin
// in others (Variation), with waves running along the wind (Gustiness).
float PrecipShowers(float2 q, float2 wind, float time)
{
    const float n = ShapeNoise(float3(q * 0.07, 0.61 + time * 0.003)).r;
    const float showers = lerp(1.0, smoothstep(0.25, 0.75, n) * 2.0, g_flVariation);
    const float phase = dot(q, wind) * 0.9 - time * 0.35 + n * 3.0;
    return max(showers * (1.0 + g_flGustiness * 0.8 * sin(phase * 6.2831853)), 0.0);
}

// Virga: the precipitation thinning as it falls (share: 0 at the cloud base, 1 at the fall's end), gone before the
// ground at high Evaporation.
float PrecipVirga(float share)
{
    if (g_flEvaporation <= 0.0)
        return 1.0;
    const float reach = 1.0 - g_flEvaporation;
    return 1.0 - smoothstep(reach * 0.5, reach + 0.02, share);
}

// The curtain's look by kind: x how much light it sends on (the rest it swallows; before Haze Brightness), y its lobe
// (before Haze Glow), z its thickness against rain's, w how much its texture streaks (rain 1, snow's flecks 0).
float4 PrecipHazeLook()
{
    const int kind = PrecipKindNow();
    if (kind == 2)
        return float4(0.9, 0.3, 1.0, 0.0);
    if (kind == 4)
        return float4(0.8, 0.55, 0.5, 0.6);
    if (kind == 3)
        return float4(0.88, 0.6, 1.2, 0.5);
    return float4(0.9, 0.75, 1.0, 1.0);
}

// The curtain's texture at q (cloud widths, z up along the fall): falling streaks for rain, drifting flecks for snow.
float PrecipHazeTexture(float3 q, float time)
{
    const float fall = time * g_flRainSpeed;
    const float streak = DetailNoise(float3(q.xy * 4.0, q.z * 0.25 + fall)).r;
    const float fleck = DetailNoise(q * 2.5 + float3(0.0, 0.0, fall * 0.15)).g;
    return lerp(fleck, streak, PrecipHazeLook().w);
}

// A colour of the rainbow, 0 violet to 1 red.
float3 PrecipSpectrum(float x)
{
    const float h = (1.0 - x) * 0.78;
    return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

// The rainbow seen along dir (from the eye), sunDir toward the sun: the primary bow 40.3 to 42.7 degrees round the
// point opposite the sun (violet inside, red outside), the secondary 50.2 to 53.8 (reversed, fainter). Rain only; per
// unit of the sun's light scattered.
float3 PrecipRainbow(float3 dir, float3 sunDir)
{
    if (g_flRainbow <= 0.0 || PrecipKindNow() != 1)
        return 0.0;
    const float a = degrees(acos(clamp(dot(dir, -sunDir), -1.0, 1.0)));
    const float x = (a - 40.3) / 2.4;
    const float y = (53.8 - a) / 3.6;
    float3 c = 0.0;
    if (x > 0.0 && x < 1.0)
        c += PrecipSpectrum(x) * sin(x * 3.14159265);
    if (y > 0.0 && y < 1.0)
        c += PrecipSpectrum(y) * sin(y * 3.14159265) * 0.35;
    return c * g_flRainbow * 3.0;
}
