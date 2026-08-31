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
        /* manufacturer */            "Akai",
        /* yearIntroduced */          1986,
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
        /* manufacturer */            "Akai",
        /* yearIntroduced */          1988,
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
        /* manufacturer */            "Akai",
        /* yearIntroduced */          1988,
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
        /* manufacturer */            "Akai",
        /* yearIntroduced */          1994,
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
        /* manufacturer */            "Akai",
        /* yearIntroduced */          1994,
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
        /* manufacturer */            "Akai",
        /* yearIntroduced */          1994,
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
    // AkzMachine_SP1200 -- E-mu SP-1200 (1987). Sources: service manual
    // (archive.org/details/emu-sp-1200-service-manual-1987), no MAME
    // driver exists for this machine at all. Heritage-roster plan
    // research pass.
    {
        /* name */                    "SP-1200",
        /* stableId */                "emu.sp1200",
        /* manufacturer */            "E-mu",
        /* yearIntroduced */          1987,
        /* minSampleRateHz */         26040.0,          // fixed -- "sample period is fixed at (1/26.04)kHz" [M]
        /* maxSampleRateHz */         26040.0,
        /* hasVariableSampleRate */   0,
        /* aaFilterCutoffRatio */     0.55,             // [I] -- manual says "on the order of 42dB/oct, cutoff less than half the sample rate"; exact ratio not given, schematic sheets needed
        /* aaFilterPoles */           7,                // 42dB/oct -> 7 poles [M for the slope; pole count is this project's one-pole-cascade approximation, same caveat as every OnePoleCascade machine]
        /* bitDepth */                12,               // "12 bit linear encoding for the sound data" [M]
        /* companded */               0,                // 12-bit linear, no companding [M]
        /* filterHasResonance */      0,                // main signal path has no resonance control -- SSM2044 is an optional COLOUR stage on channels 1-2 only, not modelled here [M]
        /* filterSlopeDbPerOctave */  42.0,             // matches the cited AA slope; output reconstruction assumed same order [I]
        /* filterTracksPitch */       0,                // trimpot-set per the manual, not pitch-tracked [M]
        /* filterTopology */          AkzFilterTopology_OnePoleCascade,
        /* filterStageCount */        1,
        /* filterResonanceCompensation01 */ 0.0,        // unused -- no resonance
        /* dacClockTracksPitch */     0,                // output clock stays fixed at 26.04kHz; pitch is purely a phase-accumulator read-rate, not a varying DAC clock [M]
        /* interpolatorOrder */       1,                // zero-order hold -- "the Pitch numbers are loaded into the Increment Latch," drop-sample against a fixed output clock, the source of its characteristic aliasing [M]
        /* supportsTimeStretch */     0,                // no time-stretch capability [M]
        /* maxStretchPercent */       0.0,
        /* hasModeSwitch */           0,
        /* hasZoneSelect */           0,
        /* defaultCycleLength */      0,
        /* memoryBudgetSamplePoints */static_cast<int64_t>(10.0 * 26040.0), // ~10 seconds max sampling time at 26.04kHz, a widely-documented spec [I] -- not independently re-verified against the service manual in this project's own research pass
    },
    // AkzMachine_FairlightCmi2x -- Fairlight CMI IIx (~1983). Sources:
    // CMI IIx Service Manual (archive.org/details/fairlight_CMI-IIx_SERVICE_MANUAL),
    // Jim Grant's "The Fairlight Explained" (E&MM Oct 1984), CEM3320/
    // SSM2045 datasheets. Deliberately NOT sourced from MAME's
    // fairlight/cmi01a.cpp (the channel-card/DAC/filter model, and
    // exactly what this profile also models) -- everything below is
    // independently available from the manual and E&MM series, per this
    // project's clean-room convention.
    {
        /* name */                    "CMI IIx",
        /* stableId */                "fairlight.cmi2x",
        /* manufacturer */            "Fairlight",
        /* yearIntroduced */          1983,
        // Rate = 128 x the pitch of the source (one cycle fills one
        // 128-byte segment of 16384-byte waveform RAM) [M] -- a genuine
        // per-note automatic relationship, not an independent front-
        // panel control. Modelled here as a continuous range a user can
        // dial (this project's simplification of the real mechanism,
        // [I]); bounds are this project's estimate around the manual's
        // own cited example rates (14080/28160 Hz), not a manual-stated
        // absolute range.
        /* minSampleRateHz */         7040.0,           // [I] -- estimated, one octave below the manual's lower example rate
        /* maxSampleRateHz */         28160.0,          // "SAMPLE RATE 14080 HZ / 28160 HZ" example rows [M]
        /* hasVariableSampleRate */   1,
        /* aaFilterCutoffRatio */     0.5,              // [I] -- the master card's switched-resistor LPF/HPF track the sample rate [M], but no cutoff ratio is given
        /* aaFilterPoles */           2,                // [I] -- generic placeholder, pole count not given for the CMOS-4051 switched-resistor stage
        /* bitDepth */                8,                // 10-bit ADC, top 8 bits stored [M]
        /* companded */               0,                // "linear, not companded" -- explicitly contrasted with the Emulator's companding [M]
        /* filterHasResonance */      1,                // per-voice tracking VCF, CEM3320 (rev 1/2) or SSM2045 (rev 3/4) [M]
        /* filterSlopeDbPerOctave */  24.0,             // CEM3320/SSM2045-class chips are 4-pole/24dB designs [I] -- not independently re-confirmed pole count in this project's research pass
        /* filterTracksPitch */       1,                // "ratio of filter cutoff to pitch... controllable," calibrated N octaves above fundamental at -18mV/oct [M]; modelled via the same simple transposeRatio multiply every other tracking machine uses, an approximation of the real octave-calibrated law [I]
        /* filterTopology */          AkzFilterTopology_CemStateVariable,
        /* filterStageCount */        2,                // two 2-pole TptSvf-class stages in series -> 24dB/oct, same technique as S3200
        /* filterResonanceCompensation01 */ 1.0,        // [I] -- full compensation, consistent with every other resonant machine here
        /* dacClockTracksPitch */     1,                // pure varispeed by clock -- the playback rate register IS the pitch register [M]
        /* interpolatorOrder */       0,                // "no interpolation in playback" [M]
        /* supportsTimeStretch */     0,                // no time-stretch found in the research pass (Mode 1's segment looping is a different mechanism) [M]
        /* maxStretchPercent */       0.0,
        /* hasModeSwitch */           0,
        /* hasZoneSelect */           0,
        /* defaultCycleLength */      0,
        /* memoryBudgetSamplePoints */0,                // multi-channel-card memory, expandable -- not modelled as a hard constraint, same treatment as the Akai S2000 and later
    },
    // AkzMachine_Mirage -- Ensoniq Mirage (1984). Sources: ES5503 "DOC"
    // ERS (brutaldeluxe.fr), CEM3328 datasheet, service manual and DSK-8
    // schematics (image-only scans, DAC part number unverified in this
    // project's research pass -- flagged, not guessed).
    {
        /* name */                    "Mirage",
        /* stableId */                "ensoniq.mirage",
        /* manufacturer */            "Ensoniq",
        /* yearIntroduced */          1984,
        /* minSampleRateHz */         10000.0,          // "variable ~10-33kHz" [M] -- approximate range from the DOC ERS, not a single precise pair of bounds
        /* maxSampleRateHz */         33000.0,
        /* hasVariableSampleRate */   1,
        /* aaFilterCutoffRatio */     0.5,              // [I] -- no citation for the DOC's own input ADC anti-alias stage specifically
        /* aaFilterPoles */           2,                // [I] -- generic placeholder
        /* bitDepth */                8,                // "8-bit unsigned wavetable data," linear, no companding [M]
        /* companded */               0,
        /* filterHasResonance */      1,                // 8x CEM3328, 4-pole 24dB/oct per voice, datasheet-confirmed [M]
        /* filterSlopeDbPerOctave */  24.0,             // CEM3328 datasheet [M]
        /* filterTracksPitch */       1,                // "with keyboard tracking," explicitly cited for the CEM3328 usage [M]
        /* filterTopology */          AkzFilterTopology_CemStateVariable,
        /* filterStageCount */        2,                // two 2-pole TptSvf-class stages in series -> 24dB/oct, same technique as S3200/Fairlight
        /* filterResonanceCompensation01 */ 1.0,        // [I] -- full compensation
        /* dacClockTracksPitch */     0,                // the ES5503 DOC generates every voice from ONE shared clock via per-oscillator phase accumulators, not a per-voice varying output clock -- pitch is handled digitally, same architecture class as the Akai S2000/S3000/S3200 [M/I]
        /* interpolatorOrder */       1,                // phase-accumulator drop-sample, zero-order hold -- no interpolation in the DOC [M]
        /* supportsTimeStretch */     0,                // no time-stretch [M]
        /* maxStretchPercent */       0.0,
        /* hasModeSwitch */           0,
        /* hasZoneSelect */           0,
        /* defaultCycleLength */      0,
        /* memoryBudgetSamplePoints */0,                // not independently verified in this project's research pass -- not modelled as a hard constraint rather than guessed
    },
    // AkzMachine_EmulatorII -- E-mu Emulator II (1984). Sources: EII
    // Service Manual (archive.org/details/e-mu_Emulator_II_Service_Manual),
    // Sound on Sound retrospective, AM6070/AM6072 mu-law DAC datasheet
    // family. MAME's src/mame/emusys/emu2.cpp is BSD-3-Clause,
    // skeleton-only (MACHINE_NO_SOUND) -- nothing audio-relevant there
    // to have drawn from either way.
    {
        /* name */                    "Emulator II",
        /* stableId */                "emu.emulator2",
        /* manufacturer */            "E-mu",
        /* yearIntroduced */          1984,
        /* minSampleRateHz */         27700.0,          // fixed ~27.7kHz [M]
        /* maxSampleRateHz */         27700.0,
        /* hasVariableSampleRate */   0,
        /* aaFilterCutoffRatio */     0.5,              // [I] -- no citation distinct from the per-channel SSM2045 covering this specifically
        /* aaFilterPoles */           4,                // [I] -- generic placeholder
        /* bitDepth */                8,                // 8-bit stored, companded -- AM6072 [M]
        /* companded */               1,                // AM6072 mu-255-style companding DAC, 15-segment (sign + 3-bit chord + 4-bit step), ~12-13 bit equivalent range from 8 stored bits [M] -- modelled via ConverterModel's standard ITU G.711 mu-law, [I] the closest well-documented approximation to the AM6072's exact segment breakpoints
        /* filterHasResonance */      1,                // SSM2045, "4 pole lowpass filter, one per channel" [M]
        /* filterSlopeDbPerOctave */  24.0,             // SSM2045 is a 4-pole/24dB ladder design, same family as SSM2044 [M]
        /* filterTracksPitch */       0,                // no citation found that the VCF cutoff tracks pitch on this machine specifically [I, absence of a citation rather than a citation of absence]
        /* filterTopology */          AkzFilterTopology_SsmLadder,
        /* filterStageCount */        1,                // SsmLadder is natively 4-pole -- one stage, not two
        /* filterResonanceCompensation01 */ 1.0,        // [I] -- full compensation
        /* dacClockTracksPitch */     1,                // "per-voice varispeed... each channel's DAC refreshed at a pitch-dependent rate" [M]
        /* interpolatorOrder */       0,                // per-voice DAC clock varied directly, same architecture class as the Akai S900/S950 -- no separate digital interpolation stage [I]
        /* supportsTimeStretch */     0,                // no time-stretch [M]
        /* maxStretchPercent */       0.0,
        /* hasModeSwitch */           0,
        /* hasZoneSelect */           0,
        /* defaultCycleLength */      0,
        /* memoryBudgetSamplePoints */0,                // not independently verified in this project's research pass -- not modelled as a hard constraint rather than guessed
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
    // AkzMachine_SP1200
    {
        { AkzProvenanceLevel_Inferred, "Fixed 26.04kHz sample rate is manual-cited (\"sample period is fixed at (1/26.04)kHz\"); the anti-alias filter's exact cutoff ratio/pole count are this project's reading of \"on the order of 42dB/oct\", not schematic-confirmed." },
        { AkzProvenanceLevel_Manual, "\"12 bit linear encoding for the sound data\" -- SP-1200 service manual, no companding." },
        { AkzProvenanceLevel_Inferred, "One-pole-cascade approximates the cited ~42dB/oct slope; not a precision filter design, same caveat as every OnePoleCascade machine -- see FilterModel.h." },
        { AkzProvenanceLevel_Manual, "Phase-accumulator drop-sample against a fixed output clock -- \"the Pitch numbers are loaded into the Increment Latch\" -- SP-1200 service manual." },
        { AkzProvenanceLevel_Unmodelled, "No time-stretch capability." },
        { AkzProvenanceLevel_Manual, "Output clock stays fixed at 26.04kHz regardless of pitch -- SP-1200 service manual." },
    },
    // AkzMachine_FairlightCmi2x
    {
        { AkzProvenanceLevel_Inferred, "\"Rate = 128 x the pitch of the source\" is manual-cited, but this project models it as a dialable knob (a simplification of the real automatic per-note mechanism) over an estimated range around the manual's own 14080/28160 Hz example rows." },
        { AkzProvenanceLevel_Manual, "10-bit ADC, top 8 bits stored, linear -- explicitly contrasted with the Emulator's companding in Jim Grant's \"The Fairlight Explained,\" E&MM Oct 1984." },
        { AkzProvenanceLevel_Inferred, "Per-voice tracking VCF (CEM3320/SSM2045) manual-confirmed, but the exact octave-calibrated -18mV/oct law is approximated here via the same simple transposeRatio multiply every other tracking machine uses; pole count assumed from the chip family, not independently re-confirmed." },
        { AkzProvenanceLevel_Manual, "\"No interpolation in playback\" -- CMI IIx service manual." },
        { AkzProvenanceLevel_Unmodelled, "No time-stretch found in this project's research pass; Mode 1's segment looping is a different mechanism." },
        { AkzProvenanceLevel_Manual, "Pure varispeed by clock -- the playback rate register IS the pitch register, CMI IIx service manual." },
    },
    // AkzMachine_Mirage
    {
        { AkzProvenanceLevel_Inferred, "\"Variable ~10-33kHz\" from the ES5503 DOC ERS is an approximate range, not a single manual-stated pair of bounds; the ADC's own anti-alias filter has no citation in this project's research pass." },
        { AkzProvenanceLevel_Manual, "\"8-bit unsigned wavetable data,\" linear, no companding -- ES5503 DOC ERS." },
        { AkzProvenanceLevel_Manual, "8x CEM3328, 4-pole 24dB/oct per voice \"with keyboard tracking\" -- CEM3328 datasheet." },
        { AkzProvenanceLevel_Manual, "Phase-accumulator drop-sample, zero-order hold, no interpolation -- ES5503 DOC ERS." },
        { AkzProvenanceLevel_Unmodelled, "No time-stretch." },
        { AkzProvenanceLevel_Inferred, "The DOC generates every voice from one shared clock via per-oscillator phase accumulators, not a per-voice varying output clock -- this project's architectural reading of the ES5503 DOC ERS, not a direct citation that the DAC ignores pitch." },
    },
    // AkzMachine_EmulatorII
    {
        { AkzProvenanceLevel_Inferred, "Fixed ~27.7kHz sample rate is manual-cited; the anti-alias filter's cutoff ratio/pole count have no citation distinct from the per-channel SSM2045." },
        { AkzProvenanceLevel_Manual, "AM6072 mu-255-style companding DAC, 15-segment (sign + 3-bit chord + 4-bit step), ~12-13 bit equivalent range from 8 stored bits -- EII service manual, AM6070/AM6072 datasheet family. Modelled via ConverterModel's standard ITU G.711 mu-law, the closest well-documented approximation to the AM6072's exact segment breakpoints, not independently verified against them." },
        { AkzProvenanceLevel_Manual, "SSM2045, \"4 pole lowpass filter, one per channel\" -- EII service manual." },
        { AkzProvenanceLevel_Inferred, "Per-voice DAC clock varied directly, same architecture class as the Akai S900/S950 -- no separate digital interpolation stage; this project's inference, not a direct citation for this machine specifically." },
        { AkzProvenanceLevel_Unmodelled, "No time-stretch." },
        { AkzProvenanceLevel_Manual, "\"Per-voice varispeed... each channel's DAC refreshed at a pitch-dependent rate\" -- EII service manual." },
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

size_t akz_machine_count(void) {
    return static_cast<size_t>(AkzMachine_Count);
}

const AkzStageProvenance* akz_machine_stage_provenance(AkzMachine machine, AkzStage stage) {
    return &akz::stageProvenance(machine, stage);
}
