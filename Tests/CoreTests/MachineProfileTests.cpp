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

AKZ_TEST(no_machine_compands) {
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        AKZ_CHECK(p->companded == 0);
    }
}

AKZ_TEST(only_S900_and_S950_have_a_pitch_tracking_filter) {
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        bool shouldTrack = (m == AkzMachine_S900 || m == AkzMachine_S950);
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

AKZ_TEST(S3200_is_the_only_machine_with_two_filter_stages) {
    // Replaces the old ">= 24.0 dB/oct" heuristic -- see AkaizerCore.h's
    // filterStageCount doc comment.
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        int expected = (m == AkzMachine_S3200) ? 2 : 1;
        AKZ_CHECK_EQ(p->filterStageCount, expected);
    }
}

AKZ_TEST(filter_topology_matches_filter_has_resonance) {
    // filterHasResonance is a UI capability flag; filterTopology is what
    // FilterModel.cpp actually dispatches on. As of v2 stage 2 (before
    // stage 6's TPT migration) they must still agree for all six Akai
    // machines: OnePoleCascade <-> no resonance, ChamberlinSvf <->
    // resonance.
    for (int m = 0; m < AkzMachine_Count; ++m) {
        const AkzMachineProfile* p = akz_machine_profile(static_cast<AkzMachine>(m));
        if (p->filterHasResonance) {
            AKZ_CHECK(p->filterTopology == AkzFilterTopology_ChamberlinSvf);
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

AKZ_TEST(default_params_are_no_op_and_machine_specific) {
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S950, &params);
    AKZ_CHECK_NEAR(params.timeFactorPercent, 100.0, 0.001); // 100% = no change
    AKZ_CHECK_EQ(params.cycleLengthSamples, 1000);          // S950 D-time default

    akz_stretch_params_default(AkzMachine_S2000, &params);
    AKZ_CHECK_EQ(params.cycleLengthSamples, 1340);          // S2000 CYC LENGTH default
}
