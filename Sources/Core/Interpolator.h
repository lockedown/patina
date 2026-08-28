// Interpolator.h
//
// Build order stage 5: per-machine transpose. This is varispeed --
// reading through a buffer at a different rate changes pitch AND
// duration together, unlike the TIME STRETCH engine elsewhere in this
// project, which decouples them on purpose. That's authentic: on the
// real hardware, transposition changes playback rate directly (S900/S950
// literally vary the per-voice DAC clock; S1000+ resample digitally to a
// fixed output rate), so a longer/shorter result at a new pitch is
// exactly what the original machines did too.
//
// Two interpolator kinds, chosen per machine from AkzMachineProfile's
// interpolatorOrder (plan section 3, "3.1 What actually differs" /
// "Known gaps"):
//
//   - ZeroOrderHold: nearest-sample playback, no interpolation between
//     samples. This is the S2000/S3000/S3200 voice chip's actual
//     behaviour -- confirmed by reading the reverse-engineered MAME
//     device `l7a1045_l6028_dsp_a.cpp`: `pos += frac >> 12; frac &=
//     0xfff;` discards the fractional bits when addressing memory. This
//     is what gives those machines their characteristic stair-stepped
//     aliasing on upward transposition.
//   - Linear: used for two different reasons on two different machines.
//     S1000's actual interpolator order is unstated by Akai (the manual
//     only gives "24-bit algorithm, custom VLSI" -- arithmetic
//     precision, not filter order); linear is this project's documented
//     assumption pending a by-ear revision. S900/S950 have NO digital
//     interpolator at all -- they vary an analog clock, so there is no
//     digital interpolation artifact to emulate in the first place.
//     Linear is used there as the cleanest simple stand-in for "no
//     interpolation error," not as a claim about what the hardware
//     literally computes.

#ifndef AKAIZER_INTERPOLATOR_H
#define AKAIZER_INTERPOLATOR_H

#include "include/AkaizerCore.h"
#include <cstddef>
#include <vector>

namespace akz {

enum class InterpolatorKind {
    Linear,
    ZeroOrderHold
};

// Looks up which kind a machine uses, from its MachineProfile entry.
InterpolatorKind interpolatorKindForMachine(AkzMachine machine);

// The output length resample() would produce for the given input length
// and ratio, without doing the resampling work. ratio > 1 raises pitch
// and shortens the result; ratio < 1 lowers pitch and lengthens it.
size_t resampledLength(size_t inLen, double ratio);

// Resamples `input` (length inLen) by `ratio`: the read position advances
// through the input by `ratio` samples per output sample. Returns exactly
// resampledLength(inLen, ratio) samples. A ratio of 1.0 (0 semitones) is
// deliberately a no-op-length operation regardless of kind.
std::vector<float> resample(const float* input, size_t inLen, double ratio, InterpolatorKind kind);

// Convenience: ratio for a semitone offset, 2^(semitones/12).
double semitonesToRatio(float semitones);

} // namespace akz

#endif // AKAIZER_INTERPOLATOR_H
