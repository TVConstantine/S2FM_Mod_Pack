#pragma once
// Which stage each technique runs at, worked out by itself (spec "Stage assignment"; plan task 10):
//   1. a table of known effects, by technique or file name: AO, GI and reflections at A (on the lit scene, before
//      the sky, fog and see-through surfaces); DOF, anti-aliasing, bloom and clarity at B (every refinement
//      sample, after S2FM's post); grading, LUTs, sharpening, grain and the like at C (the finished picture, once);
//   2. otherwise a technique that samples the depth goes to B, any other to C;
//   3. a technique that writes a texture another one reads is pulled to the reader's stage when that is earlier
//      (Launchpad's motion and normals for MXAO and RTGI).
#include <string>
#include <vector>

namespace rs {

enum { STAGE_A = 0, STAGE_B = 1, STAGE_C = 2 };

struct technique_facts {
    std::string name, file;
    bool readsDepth = false;
    std::vector<std::string> reads, writes;   // textures by declared name
};

// The stage from the table, or -1 when the effect is not in it.
int known_stage(const std::string& name, const std::string& file);
// The stage of each technique, in the order given (the preset's).
std::vector<int> assign_stages(const std::vector<technique_facts>& techniques);
// "A", "B", "C".
const char* stage_letter(int stage);

}  // namespace rs
