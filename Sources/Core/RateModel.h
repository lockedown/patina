// RateModel.h
//
// v2 heritage-roster plan, stage 4: sample-rate/bandwidth front end.
// Clears the largest v1 gap flagged in ConverterModel.h -- "the S900/
// S950 variable sample rate/bandwidth control is not modelled" -- and is
// the shared primitive every non-Akai machine needs even more than the
// Akai six do: SP-1200 (fixed 26.04 kHz), the Emulators (27.7/variable
// kHz), Fairlight (rate = 128 x pitch) and Mirage (variable ~10-33 kHz)
// are defined by their rate and anti-alias character more than by bit
// depth.
//
// Modelled as an ADC-side stage: anti-alias filter -> true decimation to
// the effective rate -> bit-depth quantise -> zero-order-hold
// reconstruction back to the host rate. The decimate+reconstruct pair is
// implemented as one combined "sample-and-hold at the machine's rate,
// expressed directly at host rate" pass (applyRecordPath below) rather
// than a literal shrink-then-grow through an intermediate shorter
// buffer -- mathematically identical, but length-neutral BY
// CONSTRUCTION rather than by care, which is what keeps
// StretchEngine::outputLength()'s mirrored arithmetic and the realtime
// player's two-phase publish protocol both untouched (both hard-assume
// every stage before the filter preserves length exactly).
//
// The anti-alias filter's cutoff TRACKS the target rate
// (cutoffHz = effectiveRateHz * aaFilterCutoffRatio) rather than sitting
// at a fixed Hz value, matching how the real S900/S950 bandwidth control
// is documented to move the sample clock and the input filter together
// (MachineProfile.cpp's filterTracksPitch rationale) -- and matching the
// same "tracking filter" pattern documented on the Fairlight CMI and
// Roland S-550's TVF, for when those land. A shallow aaFilterPoles count
// or a cutoff ratio well above 0.5 (Nyquist) is what gives a coarse ADC
// its characteristic foldover; that "deficiency" is real character, not
// a bug to fix -- see AkaizerCore.h's field comments for what's cited vs
// this project's inference per machine.

#ifndef AKAIZER_RATE_MODEL_H
#define AKAIZER_RATE_MODEL_H

#include "include/AkaizerCore.h"
#include <cstddef>

namespace akz {

// Resolves AkzStretchParams.sampleRateHz into an actual rate to run the
// record path at. 0 (or any non-positive value) resolves to
// profile.maxSampleRateHz -- the machine's own top-end rate -- per
// AkaizerCore.h's field doc. 2.1 feedback: the rate stage must never be
// bypassed, "as this is the essence of the old sampler sound"; before
// this, <= 0 resolved to hostSampleRateHz (a true bypass), and every
// machine's own default was 0 (StretchEngine.cpp's
// akz_stretch_params_default), so every machine shipped bypassed,
// including fixed-rate ones (SP-1200, Emulator II) whose one rate IS
// their defining character. There is no bypass value left: this function
// always resolves to a real, in-range rate. A positive value is clamped
// into [profile.minSampleRateHz, profile.maxSampleRateHz] so a caller
// never has to already know the machine's own range, and a fixed/dual-
// rate machine (hasVariableSampleRate == 0) collapses any request
// outside its two real rates to the nearer bound automatically, since
// clamping into [min, max] does that by construction.
double resolveSampleRateHz(AkzMachine machine, float requestedSampleRateHz, double hostSampleRateHz);

// Applies the record path to `buffer` in place: anti-alias filter (only
// when effectiveRateHz < hostSampleRateHz -- skipped entirely otherwise,
// so a machine already running at its own native/host rate sees no
// filtering at all, not even a negligible one) -> decimate+reconstruct
// -> bit-depth quantise. Always exactly `count` frames in, `count`
// frames out. A no-op beyond quantisation when effectiveRateHz >=
// hostSampleRateHz (nothing to decimate).
void applyRecordPath(float* buffer, size_t count, AkzMachine machine, double effectiveRateHz, double hostSampleRateHz);

// Applies the DAC back end to `buffer` in place: zero-order-hold at
// `playbackRateHz` (the caller resolves whether that's effectiveRateHz
// itself, for a machine whose conversion clock is fixed, or
// effectiveRateHz * transposeRatio, for one whose DAC clock tracks pitch
// -- see AkzMachineProfile.dacClockTracksPitch). No anti-alias filter (a
// D/A stage doesn't alias input content the way an A/D stage does) and
// no re-quantisation (bit depth was already fixed at record time by
// applyRecordPath -- re-quantising here would double-crush). A no-op
// when playbackRateHz >= hostSampleRateHz. Length-neutral, same
// discipline as applyRecordPath -- see StretchEngine.cpp for why this
// must run before the pre-filter cache point, not after.
void applyDacPath(float* buffer, size_t count, AkzMachine machine, double playbackRateHz, double hostSampleRateHz);

} // namespace akz

#endif // AKAIZER_RATE_MODEL_H
