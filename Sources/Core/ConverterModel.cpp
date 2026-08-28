// ConverterModel.cpp
//
// See ConverterModel.h for the rationale and what's deliberately not
// modelled yet.

#include "ConverterModel.h"

#include <algorithm>
#include <cmath>

namespace akz {

float quantize(float sample, int bits) {
    if (bits <= 0 || bits >= 24) {
        return sample;
    }

    const float clamped = std::max(-1.0f, std::min(1.0f, sample));
    const double levels = std::pow(2.0, bits);
    const double step = 2.0 / levels; // 2^bits evenly-spaced levels across [-1, 1)

    // Standard asymmetric signed-PCM index range: [-levels/2, levels/2 - 1],
    // exactly `levels` integer codes. Clamping the VALUE after scaling
    // (rather than the index before it) is the wrong place to do this --
    // an input that rounds to exactly +1.0 sails straight past a
    // `value > 1.0` check and produces a 17th level for 4-bit, a 4097th
    // for 12-bit, and so on. Clamp the index instead so the count is
    // exactly right by construction.
    double index = std::floor(static_cast<double>(clamped) / step + 0.5); // round to nearest level, no dither
    const double maxIndex = levels / 2.0 - 1.0;
    const double minIndex = -levels / 2.0;
    if (index > maxIndex) index = maxIndex;
    if (index < minIndex) index = minIndex;

    return static_cast<float>(index * step);
}

void quantizeBuffer(float* samples, size_t count, int bits) {
    if (bits <= 0 || bits >= 24) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        samples[i] = quantize(samples[i], bits);
    }
}

} // namespace akz
