// LZMA decoding. See lzma_decode.h.
#include "lzma_decode.h"

#include <vector>

namespace {

constexpr int kModelBits = 11;
constexpr uint32_t kModelTotal = 1u << kModelBits;
constexpr int kMoveBits = 5;
constexpr uint32_t kTop = 1u << 24;
constexpr int kPosStatesMax = 1 << 4;
constexpr int kLenToPosStates = 4;
constexpr int kEndPosModel = 14;
constexpr int kFullDistances = 1 << (kEndPosModel >> 1);
constexpr int kAlignBits = 4;
constexpr int kMatchMin = 2;

using Prob = uint16_t;

struct Range {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint32_t range = 0xFFFFFFFFu, code = 0;
    bool bad = false;

    // Past the end the coder reads zeros, as the reference decoder's stream does: a stream whose output is complete
    // may have been normalized past its last byte. A damaged one shows in the output, or in the checks below.
    uint8_t Next() {
        if (p >= end) return 0;
        return *p++;
    }
    bool Init() {
        if (Next() != 0) bad = true;   // the first byte is always zero
        for (int i = 0; i < 4; ++i) code = (code << 8) | Next();
        if (code == range) bad = true;
        return !bad;
    }
    void Normalize() {
        if (range < kTop) {
            range <<= 8;
            code = (code << 8) | Next();
        }
    }
    uint32_t Bit(Prob* prob) {
        const uint32_t v = *prob;
        const uint32_t bound = (range >> kModelBits) * v;
        uint32_t bit;
        if (code < bound) {
            *prob = (Prob)(v + ((kModelTotal - v) >> kMoveBits));
            range = bound;
            bit = 0;
        } else {
            *prob = (Prob)(v - (v >> kMoveBits));
            code -= bound;
            range -= bound;
            bit = 1;
        }
        Normalize();
        return bit;
    }
    uint32_t Direct(int bits) {
        uint32_t res = 0;
        do {
            range >>= 1;
            code -= range;
            const uint32_t t = 0u - (code >> 31);
            code += range & t;
            if (code == range) bad = true;
            Normalize();
            res = (res << 1) + (t + 1);
        } while (--bits);
        return res;
    }
};

uint32_t Tree(Range& rc, Prob* probs, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; ++i) m = (m << 1) + rc.Bit(&probs[m]);
    return m - (1u << bits);
}

uint32_t TreeReverse(Range& rc, Prob* probs, int bits) {
    uint32_t m = 1, sym = 0;
    for (int i = 0; i < bits; ++i) {
        const uint32_t bit = rc.Bit(&probs[m]);
        m = (m << 1) + bit;
        sym |= bit << i;
    }
    return sym;
}

struct LenDecoder {
    Prob choice = kModelTotal / 2, choice2 = kModelTotal / 2;
    Prob low[kPosStatesMax][1 << 3];
    Prob mid[kPosStatesMax][1 << 3];
    Prob high[1 << 8];
    LenDecoder() {
        for (auto& a : low) for (Prob& x : a) x = kModelTotal / 2;
        for (auto& a : mid) for (Prob& x : a) x = kModelTotal / 2;
        for (Prob& x : high) x = kModelTotal / 2;
    }
    uint32_t Decode(Range& rc, uint32_t posState) {
        if (!rc.Bit(&choice)) return Tree(rc, low[posState], 3);
        if (!rc.Bit(&choice2)) return 8 + Tree(rc, mid[posState], 3);
        return 16 + Tree(rc, high, 8);
    }
};

}  // namespace

bool LzmaDecode(const uint8_t props[5], const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize) {
    uint32_t d = props[0];
    if (d >= 9 * 5 * 5) return false;
    const uint32_t lc = d % 9;
    d /= 9;
    const uint32_t lp = d % 5;
    const uint32_t pb = d / 5;

    Range rc;
    rc.p = in;
    rc.end = in + inSize;
    if (!rc.Init()) return outSize == 0;

    std::vector<Prob> literal((size_t)0x300 << (lc + lp), (Prob)(kModelTotal / 2));
    Prob isMatch[12 << 4], isRep[12], isRepG0[12], isRepG1[12], isRepG2[12], isRep0Long[12 << 4];
    Prob posSlot[kLenToPosStates][1 << 6];
    Prob posDecoders[1 + kFullDistances - kEndPosModel];
    Prob align[1 << kAlignBits];
    for (Prob& x : isMatch) x = kModelTotal / 2;
    for (Prob* a : { isRep, isRepG0, isRepG1, isRepG2 }) for (int i = 0; i < 12; ++i) a[i] = kModelTotal / 2;
    for (Prob& x : isRep0Long) x = kModelTotal / 2;
    for (auto& a : posSlot) for (Prob& x : a) x = kModelTotal / 2;
    for (Prob& x : posDecoders) x = kModelTotal / 2;
    for (Prob& x : align) x = kModelTotal / 2;
    LenDecoder lenDec, repLenDec;

    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0, state = 0;
    size_t pos = 0;
    const uint32_t pbMask = (1u << pb) - 1, lpMask = (1u << lp) - 1;
    while (pos < outSize) {
        const uint32_t posState = (uint32_t)pos & pbMask;
        if (!rc.Bit(&isMatch[(state << 4) + posState])) {
            // A literal, with the byte the last match would give as context once a match was seen.
            const uint32_t prev = pos ? out[pos - 1] : 0;
            Prob* probs = &literal[(size_t)0x300 * ((((uint32_t)pos & lpMask) << lc) + (prev >> (8 - lc)))];
            uint32_t sym = 1;
            if (state >= 7) {
                if (rep0 + 1 > pos) return false;
                uint32_t matchByte = out[pos - rep0 - 1];
                do {
                    const uint32_t matchBit = (matchByte >> 7) & 1;
                    matchByte <<= 1;
                    const uint32_t bit = rc.Bit(&probs[((1 + matchBit) << 8) + sym]);
                    sym = (sym << 1) | bit;
                    if (matchBit != bit) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | rc.Bit(&probs[sym]);
            out[pos++] = (uint8_t)sym;
            state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
            if (rc.bad) return false;
            continue;
        }
        uint32_t len;
        if (rc.Bit(&isRep[state])) {
            if (pos == 0) return false;
            if (!rc.Bit(&isRepG0[state])) {
                if (!rc.Bit(&isRep0Long[(state << 4) + posState])) {
                    // A single byte from the last distance.
                    state = state < 7 ? 9 : 11;
                    if (rep0 + 1 > pos) return false;
                    out[pos] = out[pos - rep0 - 1];
                    ++pos;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc.Bit(&isRepG1[state])) {
                    dist = rep1;
                } else {
                    if (!rc.Bit(&isRepG2[state])) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = repLenDec.Decode(rc, posState);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = lenDec.Decode(rc, posState);
            state = state < 7 ? 7 : 10;
            // The distance.
            const uint32_t lenState = len < kLenToPosStates - 1 ? len : kLenToPosStates - 1;
            const uint32_t slot = Tree(rc, posSlot[lenState], 6);
            if (slot < 4) {
                rep0 = slot;
            } else {
                const int direct = (int)(slot >> 1) - 1;
                uint32_t dist = (2 | (slot & 1)) << direct;
                if (slot < kEndPosModel) {
                    dist += TreeReverse(rc, posDecoders + dist - slot, direct);
                } else {
                    dist += rc.Direct(direct - kAlignBits) << kAlignBits;
                    dist += TreeReverse(rc, align, kAlignBits);
                }
                rep0 = dist;
            }
            if (rep0 == 0xFFFFFFFFu) return pos == outSize;   // the end marker
        }
        len += kMatchMin;
        if (rep0 + 1 > pos) return false;
        for (uint32_t i = 0; i < len && pos < outSize; ++i, ++pos) out[pos] = out[pos - rep0 - 1];
        if (rc.bad) return false;
    }
    return !rc.bad;
}
