// ConverterModel.h
//
// Build order stage 6, converter half. Bit-depth quantisation only --
// see plan section 3.2 item 1: "S900/S950 do not compand. 12-bit SAR,
// 12-bit packed storage... Skip any mu-law step." Every machine here
// quantises with NO dither (plan section 5.1: "No dither anywhere --
// nothing in the manual or parts list suggests otherwise").
//
// Applied as the FIRST processing stage on the loaded source, before
// time-stretch -- this models "what this file would sound like if it
// had been sampled into the machine at its native bit depth," which is
// this whole app's premise (and Akaizer's), not a mastering-stage bit
// crusher applied after the fact.
//
// Deliberately NOT modelled yet, flagged rather than silently skipped:
// the S900/S950 variable sample-rate/bandwidth control (fs = bandwidth *
// 2.5 -- plan section 3.3) and the S2000/S3000 PCM69AP DAC's rising
// low-level distortion (plan section 3.3, "−34…−46 dB at −60 dB"). Both
// are real, documented character; both need more design/UI work than
// this stage budgeted for. See README's Status section for the honest
// accounting.

#ifndef AKAIZER_CONVERTER_MODEL_H
#define AKAIZER_CONVERTER_MODEL_H

#include <cstddef>

namespace akz {

// Quantises one sample to the nearest of 2^bits evenly-spaced levels
// spanning [-1, 1), no dither, and clamps to that range first. bits <= 0
// or >= 24 returns the sample unchanged (nothing meaningful to quantise
// to, and no machine here needs it).
float quantize(float sample, int bits);

// In-place buffer version of quantize().
void quantizeBuffer(float* samples, size_t count, int bits);

} // namespace akz

#endif // AKAIZER_CONVERTER_MODEL_H
