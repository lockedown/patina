// ConverterModel.cpp
//
// See ConverterModel.h for the rationale and what's deliberately not
// modelled yet.

#include "ConverterModel.h"

#include <algorithm>
#include <cmath>

namespace akz {

namespace {

// ITU G.711 standard mu-law constant -- see ConverterModel.h's
// Companding::MuLaw doc comment for why this stands in for the AM6072's
// real segment law rather than its exact breakpoints.
constexpr double kMuLawMu = 255.0;

// Compresses x in [-1,1] toward 0 logarithmically -- more resolution for
// quiet signal, less for loud, which is companding's whole point:
// spending a fixed bit budget where the ear (and the tape/DAC noise
// floor) needs it most.
double muLawCompress(double x) {
    const double sign = x < 0.0 ? -1.0 : 1.0;
    const double magnitude = std::min(1.0, std::fabs(x));
    return sign * std::log(1.0 + kMuLawMu * magnitude) / std::log(1.0 + kMuLawMu);
}

// Exact inverse of muLawCompress -- expands a compressed-and-quantised
// value back toward the linear domain the rest of the signal chain
// expects, leaving behind exactly the segment law's characteristic
// distortion (courser steps at high magnitude) as the only trace that
// companding happened at all.
double muLawExpand(double y) {
    const double sign = y < 0.0 ? -1.0 : 1.0;
    const double magnitude = std::min(1.0, std::fabs(y));
    return sign * (std::pow(1.0 + kMuLawMu, magnitude) - 1.0) / kMuLawMu;
}

// The original quantize()'s exact math (plan section 5.1: no dither
// anywhere), extracted so both the plain-bits and ConverterSpec entry
// points share one implementation. Operates on an already-clamped
// value in [-1,1] -- callers clamp first, since companding needs the
// clamp to happen before compression, not after.
double quantizeLinear(double clamped, int bits) {
    const double levels = std::pow(2.0, bits);
    const double step = 2.0 / levels; // 2^bits evenly-spaced levels across [-1, 1)

    // Standard asymmetric signed-PCM index range: [-levels/2, levels/2 - 1],
    // exactly `levels` integer codes. Clamping the VALUE after scaling
    // (rather than the index before it) is the wrong place to do this --
    // an input that rounds to exactly +1.0 sails straight past a
    // `value > 1.0` check and produces a 17th level for 4-bit, a 4097th
    // for 12-bit, and so on. Clamp the index instead so the count is
    // exactly right by construction.
    double index = std::floor(clamped / step + 0.5); // round to nearest level, no dither
    const double maxIndex = levels / 2.0 - 1.0;
    const double minIndex = -levels / 2.0;
    if (index > maxIndex) index = maxIndex;
    if (index < minIndex) index = minIndex;

    return index * step;
}

} // namespace

float quantize(float sample, const ConverterSpec& spec) {
    if (spec.bits <= 0 || spec.bits >= 24) {
        return sample;
    }

    double working = static_cast<double>(std::max(-1.0f, std::min(1.0f, sample)));
    if (spec.companding == Companding::MuLaw) {
        working = muLawCompress(working);
    }
    working = quantizeLinear(working, spec.bits);
    if (spec.companding == Companding::MuLaw) {
        working = muLawExpand(working);
    }
    return static_cast<float>(working);
}

void quantizeBuffer(float* samples, size_t count, const ConverterSpec& spec) {
    if (spec.bits <= 0 || spec.bits >= 24) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        samples[i] = quantize(samples[i], spec);
    }
}

float quantize(float sample, int bits) {
    return quantize(sample, ConverterSpec{bits});
}

void quantizeBuffer(float* samples, size_t count, int bits) {
    quantizeBuffer(samples, count, ConverterSpec{bits});
}

} // namespace akz
