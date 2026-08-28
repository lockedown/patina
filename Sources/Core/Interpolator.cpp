// Interpolator.cpp
//
// See Interpolator.h for the rationale. Nothing here should need to
// change often -- if the S1000's real interpolator order is ever
// pinned down (see the header comment's "Known gaps" reference), that's
// a one-line change to interpolatorKindForMachine, not to the resampling
// math itself.

#include "Interpolator.h"
#include "MachineProfile.h"

#include <algorithm>
#include <cmath>

namespace akz {

InterpolatorKind interpolatorKindForMachine(AkzMachine machine) {
    const AkzMachineProfile& profile = machineProfile(machine);
    // interpolatorOrder: 0 = none (S900/S950, analog clock -- no digital
    // interpolator to emulate), 1 = zero-order hold (S2000/S3000/S3200,
    // MAME-confirmed), 2 = linear (S1000, assumed). Only order 1 gets the
    // stair-stepped treatment; 0 and 2 both resolve to Linear here for
    // the reasons in the header comment.
    return profile.interpolatorOrder == 1 ? InterpolatorKind::ZeroOrderHold : InterpolatorKind::Linear;
}

size_t resampledLength(size_t inLen, double ratio) {
    if (ratio <= 0.0 || inLen == 0) {
        return 0;
    }
    return static_cast<size_t>(std::floor(static_cast<double>(inLen) / ratio));
}

double semitonesToRatio(float semitones) {
    return std::pow(2.0, static_cast<double>(semitones) / 12.0);
}

std::vector<float> resample(const float* input, size_t inLen, double ratio, InterpolatorKind kind) {
    const size_t outLen = resampledLength(inLen, ratio);
    std::vector<float> out(outLen);
    if (inLen == 0) {
        return out;
    }

    double readPos = 0.0;
    for (size_t i = 0; i < outLen; ++i) {
        const size_t idx0 = std::min(static_cast<size_t>(readPos), inLen - 1);

        if (kind == InterpolatorKind::ZeroOrderHold) {
            // Nearest-sample playback: the fractional part of readPos is
            // discarded entirely, matching the MAME device's
            // `frac &= 0xfff` after using only the integer part to
            // address memory.
            out[i] = input[idx0];
        } else {
            const size_t idx1 = std::min(idx0 + 1, inLen - 1);
            const double frac = readPos - static_cast<double>(idx0);
            out[i] = static_cast<float>(input[idx0] * (1.0 - frac) + input[idx1] * frac);
        }

        readPos += ratio;
    }
    return out;
}

} // namespace akz
