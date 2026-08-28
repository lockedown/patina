// FilterModel.h
//
// Build order stage 6, filter half. Two filter types, chosen per machine
// from AkzMachineProfile (plan section 3.2 items 2-3, section 3.4):
//
//   - OnePoleLowpassCascade: S900/S950/S1000 -- none of these have
//     resonance ("only S900/S950/S1000 lack resonance... the S2000
//     DOES have resonance, contra common belief"). Cascaded identical
//     one-pole stages, poles = filterSlopeDbPerOctave/6 (6 for the
//     S900/S950 analog 36dB/oct, 3 for the S1000's digital 18dB/oct).
//     This is an approximation of the real analog/digital response
//     shape, not a precision Butterworth design -- see the note in
//     FilterModel.cpp. On S900/S950 only, the cutoff scales with the
//     transpose ratio (filterTracksPitch), because the real per-voice
//     filter's clock is the SAME clock that sets playback rate.
//
//   - ChamberlinSVF: S2000/S3000/S3200 -- the exact difference equation
//     from the reverse-engineered MAME device `l7a1045_l6028_dsp_a.cpp`
//     (plan section 3.4): h = x - l - damping*b; b += k*h; l += k*b;
//     out = l. Resonant, cutoff fixed regardless of transpose (runs
//     after pitch interpolation on the real chip). S3200 runs two of
//     these in series (the optional "2nd DIGITAL FILTER" -- plan section
//     3, giving 24dB/oct when both are set to lowpass).
//
// Both filter kinds reset their internal state at the start of a render
// -- analogous to "filter state clears on key-on" for the SVF (the plan's
// direct citation), applied here as "each render is one new note."

#ifndef AKAIZER_FILTER_MODEL_H
#define AKAIZER_FILTER_MODEL_H

#include "include/AkaizerCore.h"
#include <cstddef>

namespace akz {

// Applies the machine-appropriate filter to `buffer` in place.
// - cutoff01: 0..1, logarithmically mapped to 20 Hz..Nyquist. 1.0 = fully
//   open (matches the hardware default "0xffff = Nyquist").
// - resonance01: 0..1, mapped to the SVF's 4-bit resonance code (0..15).
//   Ignored for machines without filterHasResonance.
// - transposeRatio: the varispeed ratio already applied by Interpolator
//   (plan section 2, "semitonesToRatio"). Only affects the result when
//   the machine's filter tracks pitch.
void applyFilter(float* buffer, size_t count, AkzMachine machine, float cutoff01, float resonance01, double sampleRateHz, double transposeRatio);

} // namespace akz

#endif // AKAIZER_FILTER_MODEL_H
