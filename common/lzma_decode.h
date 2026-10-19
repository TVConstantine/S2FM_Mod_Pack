#pragma once
// A small LZMA decoder (the "LZMA alone" stream: five property bytes, then the range coded data), for the shaders
// Half-Life: Alyx compresses inside its .vcs files. Written from the algorithm Igor Pavlov placed in the public
// domain (the LZMA SDK's LzmaSpec.cpp is its reference); no code of the SDK is copied here.
#include <cstddef>
#include <cstdint>

// Decodes a stream whose five property bytes (lc/lp/pb and the dictionary size) are props, its range coded data in
// [in, in + inSize), into exactly outSize bytes at out. False on damaged data.
bool LzmaDecode(const uint8_t props[5], const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize);
