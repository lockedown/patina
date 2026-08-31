// MachineProfile.cpp
//
// Per-machine constants, sourced from Akai owner's/service manuals and the
// MAME l7a1045_l6028_dsp_a device (used only as a behavioural reference for
// the S2000/S3000/S3200 voice chip, reimplemented rather than copied — see
// FilterModel.cpp). Every field below is annotated with where it came from;
// figures marked [I] are this project's inference, not a manual citation.
// Full citations live in the project plan.
//
// kProvenance below (v2 heritage-roster plan, stage 2) promotes those same
// [M]/[I]/[M/I] comment tags to data, one AkzStageProvenance entry per
// AkzMachine x AkzStage pair, so the app layer can surface "this is cited"
// vs. "this is this project's inference" to the user, not just to a reader
// of this file. AkzProvenanceLevel_Unmodelled means exactly that: the DSP
// doesn't implement this stage yet, independent of how good a citation for
// it might be (Rate and Dac, before stages 4/5 of the heritage-roster plan
// land, are Unmodelled for every machine here, matching the real state of
// StretchEngine.cpp regardless of what the S900/S950 manuals say about
// bandwidth).

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
        /* aaFilterCutoffRatio */     0.5,              // [I] -- inferred from the shared bandwidth-tracking design (filterTracksPitch), not itself a manual citation for the INPUT stage specifically
        /* aaFilterPoles */           6,                // [I] -- assumed same order as the cited 36dB/oct output filter (same analog block per the bandwidth control's shared design)
        /* bitDepth */                12,               // 12-bit SAR, 12-bit packed storage [M]
        /* companded */               0,                // no mu-law step anywhere in the audio path
        /* filterHasResonance */      0,                // analog SC Butterworth, no resonance control [M]
        /* filterSlopeDbPerOctave */  36.0,             // 6-pole -> 36 dB/oct
        /* filterTracksPitch */       1,                // per-voice MF6CN-50 clocked with the voice [M/I]
        /* filterTopology */          AkzFilterTopology_OnePoleCascade,
        /* filterStageCount */        1,
        /* filterResonanceCompensation01 */ 0.0,        // unused -- OnePoleCascade has no resonance
        /* dacClockTracksPitch */     1,                // per-voice DAC clock is varied directly -- same physical clock as filterTracksPitch [M/I]
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
        /* aaFilterCutoffRatio */     0.5,              // [I] -- inferred, see S900's identical note
        /* aaFilterPoles */           6,                // [I] -- assumed same order as the cited 36dB/oct output filter
        /* bitDepth */                12,               // "12-bit sampling / 16-bit processing" [M]
        /* companded */               0,
        /* filterHasResonance */      0,                // analog SC Butterworth, no resonance control [M]
        /* filterSlopeDbPerOctave */  36.0,
        /* filterTracksPitch */       1,                // MF6CN-50 per voice, clock-tracked [M/I]
        /* filterTopology */          AkzFilterTopology_OnePoleCascade,
        /* filterStageCount */        1,
        /* filterResonanceCompensation01 */ 0.0,        // unused -- OnePoleCascade has no resonance
        /* dacClockTracksPitch */     1,                // same physical clock as filterTracksPitch [M/I]
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
        /* aaFilterCutoffRatio */     0.5,              // [I] -- no manual citation for this specific stage; decimation is rarely exercised on a dual-fixed-rate machine
        /* aaFilterPoles */           4,                // [I] -- generic placeholder, same caveat
        /* bitDepth */                16,               // "16-bit linear encoding" [M]
        /* companded */               0,
        /* filterHasResonance */      0,                // "no resonance control, and the filter cannot go into self-oscillation" [M]
        /* filterSlopeDbPerOctave */  18.0,             // "Digital moving low-pass filter (-18dB/octave)" [M]
        /* filterTracksPitch */       0,                // fixed passive LC reconstruction, switched 10/20 kHz by rate, not by pitch [M]
        /* filterTopology */          AkzFilterTopology_OnePoleCascade,
        /* filterStageCount */        1,
        /* filterResonanceCompensation01 */ 0.0,        // unused -- OnePoleCascade has no resonance
        /* dacClockTracksPitch */     0,                // "fixed passive LC reconstruction... not by pitch" -- same citation as filterTracksPitch [M]
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
        /* aaFilterCutoffRatio */     0.5,              // [I] -- no manual citation for this specific stage
        /* aaFilterPoles */           4,                // [I] -- generic placeholder
        /* bitDepth */                16,
        /* companded */               0,
        /* filterHasResonance */      1,                // identical L7A1045 silicon to the S3000XL, resonant SVF -- manual-confirmed, contra common belief [M]
        /* filterSlopeDbPerOctave */  12.0,             // 2-pole Chamberlin SVF
        /* filterTracksPitch */       0,                // runs at fixed 44.1kHz after pitch interpolation [M/I from MAME device]
        /* filterTopology */          AkzFilterTopology_TptSvf,        // migrated off ChamberlinSvf in v2 stage 6 -- see AkaizerCore.h. Deliberate, accepted sonic break: fixes the ~8kHz cutoff cap and passband-gain clipping ChamberlinSvf's k<=1.1 stability clamp left behind.
        /* filterStageCount */        1,
        /* filterResonanceCompensation01 */ 1.0,        // [I] -- full compensation, since the bug this migration fixes was specifically about clipping
        /* dacClockTracksPitch */     0,                // "runs at fixed 44.1kHz after pitch interpolation" -- same citation as filterTracksPitch [M/I]
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
        /* aaFilterCutoffRatio */     0.5,              // [I] -- no manual citation for this specific stage
        /* aaFilterPoles */           4,                // [I] -- generic placeholder
        /* bitDepth */                16,
        /* companded */               0,
        /* filterHasResonance */      1,                // same L7A1045 silicon as S2000 [M]
        /* filterSlopeDbPerOctave */  12.0,
        /* filterTracksPitch */       0,
        /* filterTopology */          AkzFilterTopology_TptSvf,        // migrated off ChamberlinSvf in v2 stage 6, same rationale as S2000
        /* filterStageCount */        1,
        /* filterResonanceCompensation01 */ 1.0,        // [I] -- full compensation
        /* dacClockTracksPitch */     0,                // same voice chip as S2000, fixed clock [I]
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
        /* aaFilterCutoffRatio */     0.5,              // [I] -- no manual citation for this specific stage
        /* aaFilterPoles */           4,                // [I] -- generic placeholder
        /* bitDepth */                18,               // 18-bit converters on individual outs; stored data remains 16-bit [M/I]
        /* companded */               0,
        /* filterHasResonance */      1,                // primary L7A1045 filter, same as S2000/S3000
        /* filterSlopeDbPerOctave */  24.0,             // + optional 2nd digital filter (L7A0986 DFL) in series -> 24 dB/oct "Moog-ish" mode [M/I]
        /* filterTracksPitch */       0,
        /* filterTopology */          AkzFilterTopology_TptSvf,        // migrated off ChamberlinSvf in v2 stage 6, same rationale as S2000
        /* filterStageCount */        2,                // the optional "2nd DIGITAL FILTER," both stages lowpass in series [M/I] -- replaces the old ">= 24.0 dB/oct" heuristic
        /* filterResonanceCompensation01 */ 1.0,        // [I] -- full compensation
        /* dacClockTracksPitch */     0,                // same voice chip family, fixed clock [I]
        /* interpolatorOrder */       1,                // zero-order hold, same voice chip family [I]
        /* supportsTimeStretch */     1,
        /* maxStretchPercent */       2000.0,
        /* hasModeSwitch */           1,                // CYCLIC / INTELL [M]
        /* hasZoneSelect */           1,                // stretch zone + "to" [M]
        /* defaultCycleLength */      1000,             // Cycle length default [M]
        /* memoryBudgetSamplePoints */0,                // up to 32MB; not modelled as a hard constraint
    },
};

// Provenance table, indexed [machine][stage], order matching AkzStage
// (Rate, Converter, Filter, Interpolator, Stretch, Dac). Kept as a
// separate table rather than folded into kProfiles above because it is
// himself a fact ABOUT that table's fields, at a finer grain than any
// one field -- e.g. Filter's note differs for the same filterTopology
// value depending on which specific machine's citation quality backs it
// (S2000/S3000's ChamberlinSvf is MAME-confirmed; S900/S950's
// OnePoleCascade is flagged as an approximation in FilterModel.h's own
// header comment).
//
// A completeness test (MachineProfileTests.cpp) asserts every cell has
// a non-null note -- this table must be extended in the same commit as
// any new AkzMachine or AkzStage value, or that test fails loudly rather
// than the UI silently showing nothing for a gap.
constexpr AkzStageProvenance kProvenance[AkzMachine_Count][AkzStage_Count] = {
    // AkzMachine_S900
    {
        { AkzProvenanceLevel_Inferred, "Bandwidth range fs = bandwidth * 2.5 is manual-cited, but the tracking anti-alias filter's cutoff ratio and pole count (aaFilterCutoffRatio/aaFilterPoles) are this project's inference from the shared bandwidth-tracking design, not a citation for the INPUT stage specifically." },
        { AkzProvenanceLevel_Manual, "12-bit SAR, 12-bit packed storage, no companding -- S900 manual." },
        { AkzProvenanceLevel_Inferred, "Cascaded one-pole stages approximate the analog SC Butterworth shape; not a precision Butterworth design -- see FilterModel.h." },
        { AkzProvenanceLevel_Inferred, "No digital interpolator to emulate -- per-voice DAC clock is varied directly, this project's inference from the analog architecture." },
        { AkzProvenanceLevel_Unmodelled, "S900 has no time-stretch capability at all -- added in the S950 -- Akai manual." },
        { AkzProvenanceLevel_Manual, "DAC clock varied directly with pitch (dacClockTracksPitch) -- same per-voice architecture citation as filterTracksPitch. The zero-order-hold/reconstruction modelling itself (RateModel::applyDacPath) is this project's implementation of that citation, not a separate manual quote." },
    },
    // AkzMachine_S950
    {
        { AkzProvenanceLevel_Inferred, "Bandwidth range fs = bandwidth * 2.5 confirmed 3 ways in the manual, but the tracking anti-alias filter's cutoff ratio/pole count are this project's inference -- see S900's identical note." },
        { AkzProvenanceLevel_Manual, "\"12-bit sampling / 16-bit processing,\" no companding -- S950 manual." },
        { AkzProvenanceLevel_Inferred, "Cascaded one-pole stages approximate the analog SC Butterworth shape; not a precision Butterworth design -- see FilterModel.h." },
        { AkzProvenanceLevel_Inferred, "No digital interpolator to emulate -- per-voice DAC clock varied directly, this project's inference." },
        { AkzProvenanceLevel_Manual, "CYCLIC-only time-stretch (Mon1/Pol2, no mode switch), \"Timestretch up to 999%\" -- S950 manual. INTELLIGENT's quality/width->sample-count curve is this project's own design, not a citation." },
        { AkzProvenanceLevel_Manual, "DAC clock varied directly with pitch -- same per-voice architecture citation as filterTracksPitch." },
    },
    // AkzMachine_S1000
    {
        { AkzProvenanceLevel_Inferred, "Two fixed rates (22050/44100 Hz) are manual-cited; the anti-alias filter's cutoff ratio/pole count are an unflagged-by-the-manual generic placeholder, since decimation is rarely exercised on a dual-fixed-rate machine." },
        { AkzProvenanceLevel_Manual, "\"16-bit linear encoding,\" no companding -- S1000 manual." },
        { AkzProvenanceLevel_Inferred, "Cascade approximates the cited -18dB/oct slope; not a precision filter design -- see FilterModel.h." },
        { AkzProvenanceLevel_Inferred, "Interpolator order unstated by Akai (\"24-bit algorithm, custom VLSI\" is arithmetic precision, not filter order); linear assumed pending a by-ear revision -- README Known limitations." },
        { AkzProvenanceLevel_Manual, "CYCLIC/INTELLIGENT modes, zone select, \"25% to 2000%\" range -- S1000 manual, added OS 2.0." },
        { AkzProvenanceLevel_Manual, "\"Fixed passive LC reconstruction... not by pitch\" -- DAC clock does not track transpose, same citation as filterTracksPitch." },
    },
    // AkzMachine_S2000
    {
        { AkzProvenanceLevel_Inferred, "Two fixed rates (22050/44100 Hz) are manual-cited; the anti-alias filter's cutoff ratio/pole count are an unflagged-by-the-manual generic placeholder." },
        { AkzProvenanceLevel_Manual, "16-bit linear, no companding -- S2000 manual." },
        { AkzProvenanceLevel_Manual, "Resonant SVF, identical L7A1045 silicon to the S3000XL -- manual-confirmed, contra common belief the S2000 lacks resonance. Exact difference equation from the MAME l7a1045_l6028_dsp_a device, used as a behavioural reference and reimplemented, not copied." },
        { AkzProvenanceLevel_Manual, "Zero-order hold, MAME-confirmed (frac bits discarded when addressing memory in l7a1045_l6028_dsp_a.cpp)." },
        { AkzProvenanceLevel_Manual, "CYCLIC/INTELLIGENT modes, CYC LENGTH default 1340 -- S2000 manual. INTELLIGENT's quality/width curve is this project's own design, not a citation." },
        { AkzProvenanceLevel_Manual, "\"Runs at fixed 44.1kHz after pitch interpolation\" -- DAC clock does not track transpose, same MAME-derived citation as filterTracksPitch." },
    },
    // AkzMachine_S3000
    {
        { AkzProvenanceLevel_Inferred, "Two fixed rates (22050/44100 Hz) are manual-cited; the anti-alias filter's cutoff ratio/pole count are an unflagged-by-the-manual generic placeholder." },
        { AkzProvenanceLevel_Manual, "16-bit linear, no companding -- S3000 manual." },
        { AkzProvenanceLevel_Manual, "Same L7A1045 silicon as S2000 -- manual-confirmed resonant SVF." },
        { AkzProvenanceLevel_Manual, "Zero-order hold, same voice chip as S2000, MAME-confirmed." },
        { AkzProvenanceLevel_Manual, "CYCLIC/INTELLIGENT modes, zone select -- S3000 manual." },
        { AkzProvenanceLevel_Manual, "Same fixed-clock voice chip as S2000 -- DAC clock does not track transpose." },
    },
    // AkzMachine_S3200
    {
        { AkzProvenanceLevel_Inferred, "Two fixed rates (22050/44100 Hz) are manual-cited; the anti-alias filter's cutoff ratio/pole count are an unflagged-by-the-manual generic placeholder." },
        { AkzProvenanceLevel_Inferred, "18-bit converters on individual outs, but stored data remains 16-bit -- manual states the converter spec; how that interacts with storage is this project's reading." },
        { AkzProvenanceLevel_Inferred, "Primary L7A1045 filter manual-confirmed; the optional 2nd digital filter (L7A0986 DFL) giving 24dB/oct \"Moog-ish\" mode is a manual-cited feature whose exact combination this project infers." },
        { AkzProvenanceLevel_Manual, "Zero-order hold, same voice chip family, MAME-confirmed." },
        { AkzProvenanceLevel_Manual, "CYCLIC/INTELLIGENT modes, zone select -- S3200 manual." },
        { AkzProvenanceLevel_Manual, "Same fixed-clock voice chip family -- DAC clock does not track transpose." },
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

const AkzStageProvenance& stageProvenance(AkzMachine machine, AkzStage stage) {
    int machineIndex = static_cast<int>(machine);
    if (machineIndex < 0 || machineIndex >= static_cast<int>(AkzMachine_Count)) {
        machineIndex = static_cast<int>(AkzMachine_S950);
    }
    int stageIndex = static_cast<int>(stage);
    if (stageIndex < 0 || stageIndex >= static_cast<int>(AkzStage_Count)) {
        stageIndex = static_cast<int>(AkzStage_Filter); // sane fallback, never reached in practice
    }
    return kProvenance[machineIndex][stageIndex];
}

} // namespace akz

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

const AkzMachineProfile* akz_machine_profile(AkzMachine machine) {
    return &akz::machineProfile(machine);
}

const AkzStageProvenance* akz_machine_stage_provenance(AkzMachine machine, AkzStage stage) {
    return &akz::stageProvenance(machine, stage);
}
