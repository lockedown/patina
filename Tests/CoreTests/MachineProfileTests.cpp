// MachineProfileTests.cpp
//
// Validates the per-machine constants against the manual-derived figures
// cited in MachineProfile.cpp -- catching typos in the data table, not
// re-deriving the research itself.

#include "TestFramework.h"
#include "include/AkaizerCore.h"

#include <string>

AKZ_TEST(S950_sample_rate_matches_bandwidth_times_2_5) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_S950);
    AKZ_CHECK(p->hasVariableSampleRate == 1);
    // Bandwidth 3000-19200 Hz * 2.5 = 7500-48000 Hz, confirmed three ways
    // in the S950 owner's manual -- plan section 3.3.
    AKZ_CHECK_NEAR(p->minSampleRateHz, 3000.0 * 2.5, 0.001);
    AKZ_CHECK_NEAR(p->maxSampleRateHz, 19200.0 * 2.5, 0.001);
}

AKZ_TEST(S900_has_no_time_stretch) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_S900);
    AKZ_CHECK(p->supportsTimeStretch == 0);
}

AKZ_TEST(S950_max_stretch_is_999_not_2000) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_S950);
    AKZ_CHECK(p->supportsTimeStretch == 1);
    AKZ_CHECK_NEAR(p->maxStretchPercent, 999.0, 0.001);
    AKZ_CHECK(p->hasModeSwitch == 0); // Mon1/Pol2, not Cyclic/Intelligent
}

AKZ_TEST(S1000_and_later_reach_2000_percent) {
    const AkzMachine laterMachines[] = {AkzMachine_S1000, AkzMachine_S2000, AkzMachine_S3000, AkzMachine_S3200};
    for (AkzMachine m : laterMachines) {
        const AkzMachineProfile* p = akz_machine_profile(m);
        AKZ_CHECK_NEAR(p->maxStretchPercent, 2000.0, 0.001);
        AKZ_CHECK(p->hasModeSwitch == 1);
    }
}

AKZ_TEST(companding_matches_documented_hardware) {
    // Superseded version of a v1 test that (correctly, at the time)
    // asserted "no machine compands" as a universal. False in v2: the
    // Emulator II's AM6072 DAC compands (heritage-roster plan research)
    // -- the Akai six's own "no companding" fact survives here as six
    // entries in an explicit table, not as a universal that would now
    // be simply wrong.
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        bool shouldCompand = (m == AkzMachine_EmulatorII);
        AKZ_CHECK(static_cast<bool>(p->companded) == shouldCompand);
    }
}

AKZ_TEST(pitch_tracking_filter_matches_documented_hardware) {
    // Superseded version of a v1 test that (correctly, at the time)
    // asserted "only S900/S950" as a universal. False in v2: Fairlight
    // CMI IIx and Ensoniq Mirage both explicitly cite pitch-tracking
    // VCFs. SP-1200 does NOT join this list despite its architectural
    // similarity to S900/S950 -- its output clock stays fixed at
    // 26.04kHz regardless of pitch (a phase-accumulator read-rate
    // change, not a varying physical filter clock), matching
    // dacClockTracksPitch == 0 for the same machine.
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        bool shouldTrack = (m == AkzMachine_S900 || m == AkzMachine_S950
            || m == AkzMachine_FairlightCmi2x || m == AkzMachine_Mirage);
        AKZ_CHECK(static_cast<bool>(p->filterTracksPitch) == shouldTrack);
    }
}

AKZ_TEST(S2000_filter_has_resonance_contra_common_belief) {
    // Manual-confirmed: identical L7A1045 voice/filter silicon to the
    // S3000XL. This is the correction flagged in plan section 3.2 item 2.
    const AkzMachineProfile* s2000 = akz_machine_profile(AkzMachine_S2000);
    AKZ_CHECK(s2000->filterHasResonance == 1);

    const AkzMachineProfile* s1000 = akz_machine_profile(AkzMachine_S1000);
    AKZ_CHECK(s1000->filterHasResonance == 0); // "the filter cannot go into self-oscillation" -- manual, verbatim

    const AkzMachineProfile* s950 = akz_machine_profile(AkzMachine_S950);
    AKZ_CHECK(s950->filterHasResonance == 0); // analog SC Butterworth, no resonance control
}

AKZ_TEST(S950_memory_budget_is_rate_independent_sample_points) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_S950);
    // ~475,000 sample points regardless of rate -- verified against the
    // spec page's own seconds-at-two-rates figures, plan section 3.5.
    AKZ_CHECK(p->memoryBudgetSamplePoints > 470000);
    AKZ_CHECK(p->memoryBudgetSamplePoints < 480000);

    double secondsAt48k = static_cast<double>(p->memoryBudgetSamplePoints) / 48000.0;
    double secondsAt7500 = static_cast<double>(p->memoryBudgetSamplePoints) / 7500.0;
    AKZ_CHECK_NEAR(secondsAt48k, 9.89, 0.1);
    AKZ_CHECK_NEAR(secondsAt7500, 63.3, 1.0);
}

AKZ_TEST(every_machine_has_a_unique_nonempty_stable_id) {
    // What PresetStore.swift's AkaizerPreset.machineId compares against
    // -- a typo or duplicate here would silently misname presets.
    const char* seen[AkzMachine_Count] = {};
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        AKZ_CHECK(p->stableId != nullptr);
        AKZ_CHECK(p->stableId[0] != '\0');
        for (int j = 0; j < m; ++j) {
            AKZ_CHECK(std::string(seen[j]) != std::string(p->stableId));
        }
        seen[m] = p->stableId;
    }
}

AKZ_TEST(two_filter_stages_only_where_24db_per_octave_is_documented) {
    // Replaces the old ">= 24.0 dB/oct" heuristic -- see AkaizerCore.h's
    // filterStageCount doc comment. S3200 (Akai's own "2nd DIGITAL
    // FILTER"), Fairlight CMI IIx and Ensoniq Mirage (both CEM-class
    // 4-pole/24dB VCFs, reached via two 2-pole TptSvf-class stages in
    // series) all need two; every other machine needs one.
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        bool needsTwoStages = (m == AkzMachine_S3200 || m == AkzMachine_FairlightCmi2x || m == AkzMachine_Mirage);
        int expected = needsTwoStages ? 2 : 1;
        AKZ_CHECK_EQ(p->filterStageCount, expected);
    }
}

AKZ_TEST(filter_topology_matches_filter_has_resonance) {
    // filterHasResonance is a UI capability flag; filterTopology is what
    // FilterModel.cpp actually dispatches on. As of v2 stage 10: three
    // resonant topologies exist now (TptSvf for the Akai three,
    // CemStateVariable -- same class, different provenance -- for
    // Fairlight/Mirage, SsmLadder for the Emulator II's SSM2045), so
    // "resonant" no longer implies one specific topology the way it did
    // right after stage 6 -- only that it's NOT OnePoleCascade.
    // ChamberlinSvf itself is retired from every current profile (see
    // AkaizerCore.h; it stays defined for a future machine that
    // specifically wants its un-migrated character).
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        if (p->filterHasResonance) {
            AKZ_CHECK(p->filterTopology != AkzFilterTopology_OnePoleCascade);
            AKZ_CHECK(p->filterTopology != AkzFilterTopology_ChamberlinSvf);
        } else {
            AKZ_CHECK(p->filterTopology == AkzFilterTopology_OnePoleCascade);
        }
    }
}

AKZ_TEST(every_machine_and_stage_has_a_provenance_note) {
    // Completeness guard for the provenance table (v2 heritage-roster
    // plan, stage 2) -- a missing entry would mean the UI silently shows
    // nothing for that machine/stage rather than failing a build.
    for (int m = 0; m < AkzMachine_Count; ++m) {
        for (int s = 0; s < AkzStage_Count; ++s) {
            const AkzStageProvenance* entry = akz_machine_stage_provenance(static_cast<AkzMachine>(m), static_cast<AkzStage>(s));
            AKZ_CHECK(entry != nullptr);
            AKZ_CHECK(entry->note != nullptr);
            AKZ_CHECK(entry->note[0] != '\0');
        }
    }
}

AKZ_TEST(rate_and_dac_stages_are_no_longer_unmodelled_now_that_stages_4_and_5_landed) {
    // Superseded version of a stage-2 test that (correctly, at the time)
    // asserted Unmodelled for every machine. Heritage-roster plan stages
    // 4 (RateModel::applyRecordPath) and 5 (applyDacPath) now implement
    // both stages for all six Akai machines, so Unmodelled would be
    // FALSE here -- this is the "test suite mirrors reality" convention
    // (MachineProfile.cpp's kProvenance header comment) applied to
    // itself, not a relaxation.
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzStageProvenance* rate = akz_machine_stage_provenance(static_cast<AkzMachine>(m), AkzStage_Rate);
        const AkzStageProvenance* dac = akz_machine_stage_provenance(static_cast<AkzMachine>(m), AkzStage_Dac);
        AKZ_CHECK(rate->level != AkzProvenanceLevel_Unmodelled);
        AKZ_CHECK(dac->level != AkzProvenanceLevel_Unmodelled);
    }
}

AKZ_TEST(S900_stretch_stage_is_unmodelled_not_just_disabled_in_the_ui) {
    // supportsTimeStretch == 0 already gates the UI; provenance should
    // agree at the data level, not just describe the other five.
    const AkzStageProvenance* stretch = akz_machine_stage_provenance(AkzMachine_S900, AkzStage_Stretch);
    AKZ_CHECK(stretch->level == AkzProvenanceLevel_Unmodelled);
}

// -- v2 heritage-roster plan, stage 10: the four non-Akai machines ----

AKZ_TEST(none_of_the_new_machines_support_time_stretch) {
    // Defining fact of the whole heritage roster: they earn their place
    // through converter/filter/varispeed/rate character, not stretch.
    const AkzMachine newMachines[] = {AkzMachine_SP1200, AkzMachine_FairlightCmi2x, AkzMachine_Mirage, AkzMachine_EmulatorII};
    for (AkzMachine m : newMachines) {
        const AkzMachineProfile* p = akz_machine_profile(m);
        AKZ_CHECK(p->supportsTimeStretch == 0);
        AKZ_CHECK_NEAR(p->maxStretchPercent, 0.0, 0.001);
    }
}

AKZ_TEST(SP1200_is_fixed_at_26_04_khz_12_bit_linear) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_SP1200);
    AKZ_CHECK_NEAR(p->minSampleRateHz, 26040.0, 0.01);
    AKZ_CHECK_NEAR(p->maxSampleRateHz, 26040.0, 0.01);
    AKZ_CHECK(p->hasVariableSampleRate == 0);
    AKZ_CHECK_EQ(p->bitDepth, 12);
    AKZ_CHECK(p->companded == 0);
    AKZ_CHECK(p->filterHasResonance == 0); // main path -- SSM2044 is an optional colour stage, not modelled
    AKZ_CHECK(p->interpolatorOrder == 1);  // zero-order hold / drop-sample
    AKZ_CHECK(p->dacClockTracksPitch == 0); // output clock stays fixed regardless of pitch
}

AKZ_TEST(FairlightCmi2x_is_8_bit_linear_with_a_pitch_tracking_resonant_filter) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_FairlightCmi2x);
    AKZ_CHECK_EQ(p->bitDepth, 8);
    AKZ_CHECK(p->companded == 0); // explicitly contrasted with the Emulator's companding
    AKZ_CHECK(p->filterHasResonance == 1);
    AKZ_CHECK(p->filterTracksPitch == 1);
    AKZ_CHECK(p->filterTopology == AkzFilterTopology_CemStateVariable);
    AKZ_CHECK_EQ(p->filterStageCount, 2); // -> 24dB/oct
    AKZ_CHECK(p->interpolatorOrder == 0); // "no interpolation in playback"
    AKZ_CHECK(p->dacClockTracksPitch == 1); // pure varispeed by clock
}

AKZ_TEST(Mirage_is_8_bit_with_CEM3328_key_tracked_filter) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_Mirage);
    AKZ_CHECK_EQ(p->bitDepth, 8);
    AKZ_CHECK(p->companded == 0);
    AKZ_CHECK(p->filterHasResonance == 1);
    AKZ_CHECK(p->filterTracksPitch == 1); // CEM3328 "with keyboard tracking"
    AKZ_CHECK_NEAR(p->filterSlopeDbPerOctave, 24.0, 0.01);
    AKZ_CHECK(p->interpolatorOrder == 1); // phase-accumulator drop-sample
    AKZ_CHECK(p->dacClockTracksPitch == 0); // shared DOC clock, not per-voice
}

AKZ_TEST(EmulatorII_compands_and_uses_the_ssm_ladder_filter) {
    const AkzMachineProfile* p = akz_machine_profile(AkzMachine_EmulatorII);
    AKZ_CHECK_NEAR(p->minSampleRateHz, 27700.0, 0.01);
    AKZ_CHECK_NEAR(p->maxSampleRateHz, 27700.0, 0.01);
    AKZ_CHECK_EQ(p->bitDepth, 8);
    AKZ_CHECK(p->companded == 1); // AM6072 mu-255-style companding DAC
    AKZ_CHECK(p->filterHasResonance == 1);
    AKZ_CHECK(p->filterTopology == AkzFilterTopology_SsmLadder); // SSM2045, "4 pole lowpass filter, one per channel"
    AKZ_CHECK_EQ(p->filterStageCount, 1); // SsmLadder is natively 4-pole
    AKZ_CHECK(p->dacClockTracksPitch == 1); // per-voice varispeed DAC
}

AKZ_TEST(every_new_machine_has_a_unique_manufacturer_and_year) {
    // Sanity check on the roster-metadata fields (stage 9) for the four
    // machines that actually exercise cross-manufacturer grouping --
    // the six Akai machines all share "Akai," so this is where a copy-
    // paste error in manufacturer/year would first show up.
    const AkzMachine newMachines[] = {AkzMachine_SP1200, AkzMachine_FairlightCmi2x, AkzMachine_Mirage, AkzMachine_EmulatorII};
    const char* expectedManufacturers[] = {"E-mu", "Fairlight", "Ensoniq", "E-mu"};
    for (size_t i = 0; i < 4; ++i) {
        const AkzMachineProfile* p = akz_machine_profile(newMachines[i]);
        AKZ_CHECK(std::string(p->manufacturer) == std::string(expectedManufacturers[i]));
        AKZ_CHECK(p->yearIntroduced >= 1980);
        AKZ_CHECK(p->yearIntroduced <= 1990);
    }
}

AKZ_TEST(default_params_are_no_op_and_machine_specific) {
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S950, &params);
    AKZ_CHECK_NEAR(params.timeFactorPercent, 100.0, 0.001); // 100% = no change
    AKZ_CHECK_EQ(params.cycleLengthSamples, 1000);          // S950 D-time default

    akz_stretch_params_default(AkzMachine_S2000, &params);
    AKZ_CHECK_EQ(params.cycleLengthSamples, 1340);          // S2000 CYC LENGTH default
}

AKZ_TEST(every_machines_default_sample_rate_is_its_own_max_never_zero) {
    // 2.1 feedback: bandwidth must "never be 0 or bypassed." Roster-wide
    // guard: every machine's default sampleRateHz equals its OWN
    // maxSampleRateHz (never the old 0 sentinel), so the rate stage is
    // engaged out of the box for every machine, including the fixed-rate
    // ones whose single rate is their whole defining character.
    for (int i = 0; i < AkzMachine_Count; ++i) {
        const AkzMachine machine = static_cast<AkzMachine>(i);
        const AkzMachineProfile* profile = akz_machine_profile(machine);
        AkzStretchParams params;
        akz_stretch_params_default(machine, &params);
        AKZ_CHECK(params.sampleRateHz > 0.0f);
        AKZ_CHECK_NEAR(params.sampleRateHz, profile->maxSampleRateHz, 0.001);
    }
}
