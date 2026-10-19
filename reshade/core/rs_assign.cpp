#include "rs_assign.h"

#include <algorithm>
#include <cctype>

namespace {

std::string Upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

// Matched as substrings of the technique's or the file's name, upper case, A's list first.
const char* const kStageA[] = {
    "MXAO", "RTGI", "SPECGI", "LAUNCHPAD", "SSR", "SSDO", "GLAMAYRE", "HBAO", "GTAO", "SSAO", "MARTYSMODS_AO",
    "RADIANT", "PTGI", "DH_UBER_RT", "DH_RT",
};
const char* const kStageB[] = {
    "DOF", "BOKEH", "SMAA", "FXAA", "TAA", "ANTIALIASING", "CLARITY", "BLOOM", "SOLARIS", "XENON", "AMBIENTLIGHT",
    "MAGICHDR", "SUPERDEPTH3D", "DEPTH3D", "MOTIONBLUR", "LIGHTDOF", "GLOW", "HALATION", "FOG", "VOLUMETRIC",
};
const char* const kStageC[] = {
    "LUT", "REGRADE", "GRADING", "LUMASHARPEN", "ADAPTIVESHARPEN", "CAS", "SHARPEN", "FILMGRAIN", "GRAIN",
    "VIGNETTE", "TONEMAP", "CHROMATICITY", "LEVELS", "CURVES", "DEBAND", "DITHER", "VIBRANCE", "LIFTGAMMAGAIN",
    "TECHNICOLOR", "SEPIA", "PD80_0", "COLORMATRIX", "CHROMATICABERRATION", "LETTERBOX", "BORDER",
};

bool AnyIn(const std::string& n, const std::string& f, const char* const* list, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (n.find(list[i]) != std::string::npos || f.find(list[i]) != std::string::npos) return true;
    return false;
}

}  // namespace

int rs::known_stage(const std::string& name, const std::string& file) {
    const std::string n = Upper(name), f = Upper(file);
    if (AnyIn(n, f, kStageA, sizeof(kStageA) / sizeof(kStageA[0]))) return STAGE_A;
    if (AnyIn(n, f, kStageB, sizeof(kStageB) / sizeof(kStageB[0]))) return STAGE_B;
    if (AnyIn(n, f, kStageC, sizeof(kStageC) / sizeof(kStageC[0]))) return STAGE_C;
    return -1;
}

std::vector<int> rs::assign_stages(const std::vector<technique_facts>& t) {
    std::vector<int> stage(t.size(), STAGE_C);
    for (size_t i = 0; i < t.size(); ++i) {
        const int known = known_stage(t[i].name, t[i].file);
        stage[i] = known >= 0 ? known : (t[i].readsDepth ? STAGE_B : STAGE_C);
    }
    // A writer of a texture is pulled to the earliest stage of its readers, until nothing moves.
    for (bool moved = true; moved;) {
        moved = false;
        for (size_t w = 0; w < t.size(); ++w)
            for (size_t r = 0; r < t.size(); ++r) {
                if (r == w || stage[r] >= stage[w]) continue;
                for (const std::string& tex : t[w].writes) {
                    if (std::find(t[r].reads.begin(), t[r].reads.end(), tex) != t[r].reads.end()
                        && std::find(t[r].writes.begin(), t[r].writes.end(), tex) == t[r].writes.end()) {
                        stage[w] = stage[r];
                        moved = true;
                        break;
                    }
                }
            }
    }
    return stage;
}

const char* rs::stage_letter(int stage) { return stage == STAGE_A ? "A" : stage == STAGE_B ? "B" : "C"; }
