// MachineProfileTests.cpp
//
// Validates the per-machine constants against the manual-derived figures
// cited in MachineProfile.cpp -- catching typos in the data table, not
// re-deriving the research itself.

#include "TestFramework.h"
#include "include/AkaizerCore.h"

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

AKZ_TEST(default_params_are_no_op_and_machine_specific) {
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S950, &params);
    AKZ_CHECK_NEAR(params.timeFactorPercent, 100.0, 0.001); // 100% = no change
    AKZ_CHECK_EQ(params.cycleLengthSamples, 1000);          // S950 D-time default

    akz_stretch_params_default(AkzMachine_S2000, &params);
    AKZ_CHECK_EQ(params.cycleLengthSamples, 1340);          // S2000 CYC LENGTH default
}
