// MachineProfile.cpp
//
// Per-machine constants, sourced from Akai owner's/service manuals and the
// MAME l7a1045_l6028_dsp_a device (used only as a behavioural reference for
// the S2000/S3000/S3200 voice chip, reimplemented rather than copied — see
// FilterModel.cpp). Every field below is annotated with where it came from;
// figures marked [I] are this project's inference, not a manual citation.
// Full citations live in the project plan.

#include "MachineProfile.h"

namespace akz {

namespace {

// S900/S950: continuously variable via an "audio bandwidth" control of
// 3000-19200 Hz on the S950 (3000-16000 Hz on the S900), with
// fs = bandwidth * 2.5 confirmed three independent ways in the S950
// manual. hasVariableSampleRate machines model *bandwidth*, not a rate
// dropdown -- see StretchEngine / the app layer for where that conversion
// happens. minSampleRateHz/maxSampleRateHz below are the resulting fs
// range, not the bandwidth range.
constexpr AkzMachineProfile kProfiles[AkzMachine_Count] = {
    // AkzMachine_S900
    {
        /* name */                    "S900",
        /* stableId */                "akai.s900",
        /* minSampleRateHz */         7500.0,
        /* maxSampleRateHz */         40000.0,          // bandwidth 3000-16000 Hz * 2.5 [M]
        /* hasVariableSampleRate */   1,
        /* bitDepth */                12,               // 12-bit SAR, 12-bit packed storage [M]
        /* companded */               0,                // no mu-law step anywhere in the audio path
        /* filterHasResonance */      0,                // analog SC Butterworth, no resonance control [M]
        /* filterSlopeDbPerOctave */  36.0,             // 6-pole -> 36 dB/oct
        /* filterTracksPitch */       1,                // per-voice MF6CN-50 clocked with the voice [M/I]
        /* interpolatorOrder */       0,                // no interpolation -- per-voice DAC clock is varied directly [I]
        /* supportsTimeStretch */     0,                // added in the S950; S900 has none [M]
        /* maxStretchPercent */       0.0,
        /* hasModeSwitch */           0,
        /* hasZoneSelect */           0,
        /* defaultCycleLength */      0,
        /* memoryBudgetSamplePoints */750000 * 2 / 3,   // 750 KB at 12-bit packed (1.5 bytes/sample) [M/I]
    },
    // AkzMachine_S950
    {
        /* name */                    "S950",
        /* stableId */                "akai.s950",
        /* minSampleRateHz */         7500.0,
        /* maxSampleRateHz */         48000.0,          // bandwidth 3000-19200 Hz * 2.5, confirmed 3 ways in manual [M]
        /* hasVariableSampleRate */   1,
        /* bitDepth */                12,               // "12-bit sampling / 16-bit processing" [M]
        /* companded */               0,
        /* filterHasResonance */      0,                // analog SC Butterworth, no resonance control [M]
        /* filterSlopeDbPerOctave */  36.0,
        /* filterTracksPitch */       1,                // MF6CN-50 per voice, clock-tracked [M/I]
        /* interpolatorOrder */       0,                // no interpolation -- per-voice DAC clock varied directly [I]
        /* supportsTimeStretch */     1,
        /* maxStretchPercent */       999.0,            // "Timestretch (up to 999%)" [M]
        /* hasModeSwitch */           0,                // Mon1/Pol2 instead of Cyclic/Intelligent [M]
        /* hasZoneSelect */           0,                // whole-sample only [M]
        /* defaultCycleLength */      1000,             // D-time default [M]
        /* memoryBudgetSamplePoints */475000,           // ~475k sample points regardless of rate, verified by arithmetic against the spec page [M/I]
    },
    // AkzMachine_S1000
    {
        /* name */                    "S1000",
        /* stableId */                "akai.s1000",
        /* minSampleRateHz */         22050.0,
        /* maxSampleRateHz */         44100.0,          // exactly two rates, no continuous variation [M]
        /* hasVariableSampleRate */   0,
        /* bitDepth */                16,               // "16-bit linear encoding" [M]
        /* companded */               0,
        /* filterHasResonance */      0,                // "no resonance control, and the filter cannot go into self-oscillation" [M]
        /* filterSlopeDbPerOctave */  18.0,             // "Digital moving low-pass filter (-18dB/octave)" [M]
        /* filterTracksPitch */       0,                // fixed passive LC reconstruction, switched 10/20 kHz by rate, not by pitch [M]
        /* interpolatorOrder */       2,                // order unstated by Akai; linear assumed pending by-ear revision -- see plan "Known gaps" [I]
        /* supportsTimeStretch */     1,                // added in OS 2.0 [M]
        /* maxStretchPercent */       2000.0,           // "25% of its original length to 2000%" [M]
        /* hasModeSwitch */           1,                // CYCLIC / INTELL [M]
        /* hasZoneSelect */           1,                // stretch zone + "to" [M]
        /* defaultCycleLength */      1000,             // [F], Akaizer proxy -- no manual range given
        /* memoryBudgetSamplePoints */0,                // commodity-SIMM expandable; not modelled as a hard constraint
    },
    // AkzMachine_S2000
    {
        /* name */                    "S2000",
        /* stableId */                "akai.s2000",
        /* minSampleRateHz */         22050.0,
        /* maxSampleRateHz */         44100.0,
        /* hasVariableSampleRate */   0,
        /* bitDepth */                16,
        /* companded */               0,
        /* filterHasResonance */      1,                // identical L7A1045 silicon to the S3000XL, resonant SVF -- manual-confirmed, contra common belief [M]
        /* filterSlopeDbPerOctave */  12.0,             // 2-pole Chamberlin SVF
        /* filterTracksPitch */       0,                // runs at fixed 44.1kHz after pitch interpolation [M/I from MAME device]
        /* interpolatorOrder */       1,                // zero-order hold, per MAME l7a1045_l6028_dsp_a.cpp: frac bits discarded when addressing [I, best available evidence]
        /* supportsTimeStretch */     1,
        /* maxStretchPercent */       2000.0,
        /* hasModeSwitch */           1,                // CYCLIC / INTELLIGENT [M]
        /* hasZoneSelect */           0,                // no zone selection despite being later than S1000 [M]
        /* defaultCycleLength */      1340,             // CYC LENGTH default [M]
        /* memoryBudgetSamplePoints */0,                // up to 32MB via 72-pin SIMMs; not modelled as a hard constraint
    },
    // AkzMachine_S3000
    {
        /* name */                    "S3000",
        /* stableId */                "akai.s3000",
        /* minSampleRateHz */         22050.0,
        /* maxSampleRateHz */         44100.0,
        /* hasVariableSampleRate */   0,
        /* bitDepth */                16,
        /* companded */               0,
        /* filterHasResonance */      1,                // same L7A1045 silicon as S2000 [M]
        /* filterSlopeDbPerOctave */  12.0,
        /* filterTracksPitch */       0,
        /* interpolatorOrder */       1,                // zero-order hold, same voice chip as S2000 [I]
        /* supportsTimeStretch */     1,
        /* maxStretchPercent */       2000.0,
        /* hasModeSwitch */           1,                // CYCLIC / INTELL [M]
        /* hasZoneSelect */           1,                // stretch zone + "to", like S1000 [M]
        /* defaultCycleLength */      1000,             // Cycle length default [M]
        /* memoryBudgetSamplePoints */0,
    },
    // AkzMachine_S3200
    {
        /* name */                    "S3200",
        /* stableId */                "akai.s3200",
        /* minSampleRateHz */         22050.0,
        /* maxSampleRateHz */         44100.0,
        /* hasVariableSampleRate */   0,
        /* bitDepth */                18,               // 18-bit converters on individual outs; stored data remains 16-bit [M/I]
        /* companded */               0,
        /* filterHasResonance */      1,                // primary L7A1045 filter, same as S2000/S3000
        /* filterSlopeDbPerOctave */  24.0,             // + optional 2nd digital filter (L7A0986 DFL) in series -> 24 dB/oct "Moog-ish" mode [M/I]
        /* filterTracksPitch */       0,
        /* interpolatorOrder */       1,                // zero-order hold, same voice chip family [I]
        /* supportsTimeStretch */     1,
        /* maxStretchPercent */       2000.0,
        /* hasModeSwitch */           1,                // CYCLIC / INTELL [M]
        /* hasZoneSelect */           1,                // stretch zone + "to" [M]
        /* defaultCycleLength */      1000,             // Cycle length default [M]
        /* memoryBudgetSamplePoints */0,                // up to 32MB; not modelled as a hard constraint
    },
};

} // namespace

const AkzMachineProfile& machineProfile(AkzMachine machine) {
    int index = static_cast<int>(machine);
    if (index < 0 || index >= static_cast<int>(AkzMachine_Count)) {
        index = static_cast<int>(AkzMachine_S950); // sane fallback, never reached in practice
    }
    return kProfiles[index];
}

} // namespace akz

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

const AkzMachineProfile* akz_machine_profile(AkzMachine machine) {
    return &akz::machineProfile(machine);
}
