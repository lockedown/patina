// ConverterModel.h
//
// Build order stage 6, converter half, widened in the v2 heritage-
// roster plan's stage 7. Bit-depth quantisation, no dither on any
// current machine -- plan section 3.2 item 1: "S900/S950 do not
// compand. 12-bit SAR, 12-bit packed storage... Skip any mu-law step."
// v2 adds real companding support (ConverterSpec::companding) for the
// heritage roster's Emulator II, whose research citation names an
// AM6072 mu-255-style companding DAC -- see MachineProfile.cpp when
// that machine lands.
//
// Applied as the FIRST processing stage on the loaded source, before
// time-stretch -- this models "what this file would sound like if it
// had been sampled into the machine at its native bit depth" (this
// whole app's premise, and Akaizer's), not a mastering-stage bit
// crusher applied after the fact. Companding is modelled the same way:
// a full round trip (compress -> quantise -> expand) within one call,
// leaving the result in linear domain with the segment law's
// characteristic distortion baked in -- exactly how plain quantisation
// already round-trips, generalised rather than replaced.
//
// Deliberately NOT modelled yet, flagged rather than silently skipped:
// the S2000/S3000 PCM69AP DAC's rising low-level distortion (plan
// section 3.3, "-34...-46 dB at -60 dB") -- see TransferCurve below,
// kept as a named-but-unimplemented placeholder rather than invented
// without a citation. See README's Status section for the honest
// accounting; the S900/S950 variable sample-rate/bandwidth control
// (plan section 3.3) is no longer in this list -- see RateModel.h.

#ifndef AKAIZER_CONVERTER_MODEL_H
#define AKAIZER_CONVERTER_MODEL_H

#include <cstddef>

namespace akz {

// Non-linear coding a machine's ADC/DAC may use. Only MuLaw is
// implemented -- none of the six Akai machines compand (all cite
// companded == 0 in MachineProfile.cpp, "no mu-law step anywhere in the
// audio path"); this exists for the AM6072 mu-255-style companding DAC
// the Emulator II's research citation names (heritage-roster plan stage
// 10). Modelled as the standard ITU G.711 mu-law curve (mu = 255) --
// [I], the closest well-documented approximation to the AM6072's real
// 15-segment law, not independently verified against its exact segment
// breakpoints.
enum class Companding {
    None,
    MuLaw
};

// A DAC/ADC non-linear transfer curve distinct from companding -- e.g.
// the PCM69AP's rising low-level distortion this header's own comment
// above flags as not modelled. Linear is the only implemented value:
// adding a real curve here without a verified transfer function would
// violate this project's citation-first fidelity bar. Kept as a named
// placeholder so the day a citation exists, ConverterSpec already has
// somewhere for it to live.
enum class TransferCurve {
    Linear
};

// Everything quantize()/quantizeBuffer() need to model one machine's
// converter, replacing a bare `int bits` -- built by the caller (see
// RateModel.cpp) from AkzMachineProfile fields; this file never sees an
// AkzMachine and stays exactly as machine-unaware as it was in v1.
// Aggregate type on purpose: `ConverterSpec{bits}` (companding/curve
// default to None/Linear) is a drop-in replacement for the old bare-int
// call sites, which is exactly what quantize(sample, int bits) below
// does internally -- the plain-bits entry point isn't a separate
// implementation, it's this one with the defaults.
struct ConverterSpec {
    int bits = 16;
    Companding companding = Companding::None;
    TransferCurve curve = TransferCurve::Linear;
    float curveAmount = 0.0f; // unused while curve == Linear
    int ditherKind = 0;       // unused -- no citation for dither on any current or planned machine; kept for a future one that might
};

// Quantises one sample per `spec`: clamp to [-1,1] -> optional
// companding compress -> quantise to spec.bits evenly-spaced levels, no
// dither -> optional companding expand. bits <= 0 or >= 24 returns the
// sample unchanged (nothing meaningful to quantise to, and no machine
// here needs it) regardless of companding/curve.
float quantize(float sample, const ConverterSpec& spec);

// In-place buffer version of quantize(const ConverterSpec&).
void quantizeBuffer(float* samples, size_t count, const ConverterSpec& spec);

// Convenience entry points equivalent to ConverterSpec{bits} (no
// companding, linear curve) -- what every call site before v2 stage 7
// already looked like, and what every current machine (companded == 0)
// still resolves to.
float quantize(float sample, int bits);
void quantizeBuffer(float* samples, size_t count, int bits);

} // namespace akz

#endif // AKAIZER_CONVERTER_MODEL_H
