// Light of one wavelength as a colour: the CIE 1931 colour matching functions (the multi-lobe fit of Wyman, Sloan
// and Shirley, 2013) and CIE XYZ to linear sRGB. The thin film (lit.vfx) sums a spectrum through them; the
// holographic grating (lighting.hlsl) sends each wavelength its own way.

// One side of a colour matching function, a Gaussian with a width each side of its peak.
float Lobe(float l, float peak, float below, float above)
{
    const float t = (l - peak) / (l < peak ? below : above);
    return exp(-0.5 * t * t);
}

// x, y and z at wavelength l (nm).
float3 ColourMatching(float l)
{
    return float3(1.056 * Lobe(l, 599.8, 37.9, 31.0) + 0.362 * Lobe(l, 442.0, 16.0, 26.7) - 0.065 * Lobe(l, 501.1, 20.4, 26.2),
                  0.821 * Lobe(l, 568.8, 46.9, 40.5) + 0.286 * Lobe(l, 530.9, 16.3, 31.1),
                  1.217 * Lobe(l, 437.0, 11.8, 36.0) + 0.681 * Lobe(l, 459.0, 26.0, 13.8));
}

static const float3x3 g_XyzToRgb = float3x3(3.2404542, -1.5371385, -0.4985314,
                                            -0.9692660, 1.8760108, 0.0415560,
                                            0.0556434, -0.2040259, 1.0572252);

// The colour of light at wavelength l (nm), the parts outside sRGB cut off, scaled so that the mean over
// 400..700 nm is white (1, 1, 1): a grating that spreads white light over the spectrum gives the white back.
float3 Spectrum(float l)
{
    return max(mul(g_XyzToRgb, ColourMatching(l)), 0.0) * float3(1.7028, 2.6001, 2.7442);
}
