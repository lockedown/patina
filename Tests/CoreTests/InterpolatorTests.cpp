// InterpolatorTests.cpp
//
// Correctness for the per-machine transpose/varispeed step (build order
// stage 5). Two things worth proving, not just asserting:
//   1. Machine selection is correct: S2000/S3000/S3200 get zero-order
//      hold, S900/S950/S1000 get linear -- see Interpolator.h.
//   2. The two kinds actually produce different, distinguishable output
//      on the same input -- otherwise "zero-order hold" would be a
//      label with no behavioural difference behind it.

#include "TestFramework.h"
#include "include/AkaizerCore.h"

// Interpolator.h is an internal Core header (not part of the public C
// API), reached the same way StretchEngine.cpp reaches it.
#include "../../Sources/Core/Interpolator.h"

#include <cmath>
#include <vector>

using namespace akz;

AKZ_TEST(zero_order_hold_is_selected_only_for_S2000_S3000_S3200) {
    AKZ_CHECK(interpolatorKindForMachine(AkzMachine_S900) == InterpolatorKind::Linear);
    AKZ_CHECK(interpolatorKindForMachine(AkzMachine_S950) == InterpolatorKind::Linear);
    AKZ_CHECK(interpolatorKindForMachine(AkzMachine_S1000) == InterpolatorKind::Linear);
    AKZ_CHECK(interpolatorKindForMachine(AkzMachine_S2000) == InterpolatorKind::ZeroOrderHold);
    AKZ_CHECK(interpolatorKindForMachine(AkzMachine_S3000) == InterpolatorKind::ZeroOrderHold);
    AKZ_CHECK(interpolatorKindForMachine(AkzMachine_S3200) == InterpolatorKind::ZeroOrderHold);
}

AKZ_TEST(semitones_to_ratio_matches_equal_temperament) {
    AKZ_CHECK_NEAR(semitonesToRatio(0.0f), 1.0, 1e-9);
    AKZ_CHECK_NEAR(semitonesToRatio(12.0f), 2.0, 1e-9);   // octave up
    AKZ_CHECK_NEAR(semitonesToRatio(-12.0f), 0.5, 1e-9);  // octave down
}

AKZ_TEST(resample_length_matches_ratio) {
    AKZ_CHECK_EQ(resampledLength(1000, 1.0), static_cast<size_t>(1000));
    AKZ_CHECK_EQ(resampledLength(1000, 2.0), static_cast<size_t>(500));  // +12 semitones: half the duration
    AKZ_CHECK_EQ(resampledLength(1000, 0.5), static_cast<size_t>(2000)); // -12 semitones: double the duration
}

AKZ_TEST(zero_order_hold_output_is_a_strict_subset_of_input_values) {
    // Every output sample must equal SOME input sample exactly -- that's
    // the defining property of nearest-sample playback, and it's exactly
    // what should NOT hold for linear interpolation on a non-constant
    // signal (interpolated values fall between input samples).
    std::vector<float> ramp(100);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i);

    auto held = resample(ramp.data(), ramp.size(), 1.37, InterpolatorKind::ZeroOrderHold);
    AKZ_CHECK(!held.empty());
    for (float v : held) {
        // Ramp values are exactly their index -- a held sample must be
        // an exact integer that appeared in the source.
        AKZ_CHECK(v == std::floor(v));
        AKZ_CHECK(v >= 0.0f && v < 100.0f);
    }
}

AKZ_TEST(linear_interpolation_produces_inbetween_values_on_a_ramp) {
    std::vector<float> ramp(100);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i);

    auto interpolated = resample(ramp.data(), ramp.size(), 1.37, InterpolatorKind::Linear);
    // On a linear ramp, linear interpolation should reproduce the ramp's
    // own formula almost exactly (this is the one signal shape where
    // linear interpolation is essentially exact) -- reconstruct expected
    // values directly from the read-position formula and compare.
    double readPos = 0.0;
    bool sawNonIntegerRead = false;
    for (size_t i = 0; i < interpolated.size(); ++i) {
        double frac = readPos - std::floor(readPos);
        if (frac > 1e-9) sawNonIntegerRead = true;
        AKZ_CHECK_NEAR(interpolated[i], readPos, 1e-4);
        readPos += 1.37;
    }
    AKZ_CHECK(sawNonIntegerRead); // otherwise this test never actually exercised interpolation
}

AKZ_TEST(the_two_kinds_disagree_on_a_non_ramp_signal) {
    // A signal with real curvature (not a straight ramp) is where linear
    // interpolation and zero-order hold actually diverge -- confirms
    // the two code paths are behaviourally distinct, not just labelled
    // differently.
    std::vector<float> signal(200);
    for (size_t i = 0; i < signal.size(); ++i) {
        signal[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.3));
    }

    auto held = resample(signal.data(), signal.size(), 1.6, InterpolatorKind::ZeroOrderHold);
    auto interpolated = resample(signal.data(), signal.size(), 1.6, InterpolatorKind::Linear);

    AKZ_CHECK_EQ(held.size(), interpolated.size());
    bool anyDifference = false;
    for (size_t i = 0; i < held.size(); ++i) {
        if (std::fabs(held[i] - interpolated[i]) > 1e-6) { anyDifference = true; break; }
    }
    AKZ_CHECK(anyDifference);
}

AKZ_TEST(zero_semitones_via_stretch_engine_is_length_preserving) {
    // Integration point: AkzStretchParams.transposeSemitones defaults to
    // 0 and must be a true no-op through the whole engine, not just in
    // isolation -- this is what keeps every pre-stage-5 length test in
    // StretchEngineTests.cpp valid without modification.
    std::vector<float> source(5000);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(i % 100) / 100.0f;

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    AKZ_CHECK_NEAR(params.transposeSemitones, 0.0, 1e-9); // the default itself

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());

    size_t len = akz_stretch_engine_output_length(engine);
    AKZ_CHECK_EQ(len, source.size()); // 100% stretch, 0 semitones -> untouched length

    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(transpose_up_shortens_output_transpose_down_lengthens_it) {
    std::vector<float> source(10000);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(i % 50) / 50.0f;

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.timeFactorPercent = 100.0f; // isolate transpose from the stretch step

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);

    params.transposeSemitones = 12.0f; // +1 octave
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());
    size_t lenUp = akz_stretch_engine_output_length(engine);

    params.transposeSemitones = -12.0f; // -1 octave
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());
    size_t lenDown = akz_stretch_engine_output_length(engine);

    AKZ_CHECK_NEAR(static_cast<double>(lenUp), source.size() / 2.0, 2.0);
    AKZ_CHECK_NEAR(static_cast<double>(lenDown), source.size() * 2.0, 2.0);

    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(S2000_and_S1000_transpose_differently_end_to_end) {
    // Not just testing Interpolator.cpp in isolation -- confirms the
    // StretchEngine actually routes different machines through different
    // interpolators end to end, via the public C API exactly as the app
    // would use it. Doesn't re-assert ZOH's exact "output is a strict
    // subset of input values" property here: FilterModel's VCF now runs
    // AFTER the interpolator in the signal chain (plan "4. Signal
    // chain"), and a resonant SVF is never a true bypass at any cutoff
    // setting, so there's no way to isolate the interpolator's raw
    // output through the public API without the filter's own smoothing
    // in the way -- that exact-value property is what the isolated
    // resample()-calling tests above are for. This test's job is just to
    // confirm the machine selection actually changes end-to-end output,
    // not merely accepted as a parameter and ignored.
    std::vector<float> signal(2000);
    for (size_t i = 0; i < signal.size(); ++i) {
        signal[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.37));
    }

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = static_cast<int>(signal.size()) * 2; // one block, no crossfade seam
    params.transposeSemitones = 7.0f; // an awkward, non-octave ratio -- exercises real interpolation, not a clean integer step
    params.filterCutoff01 = 1.0f;

    AkzStretchEngine* s1000Engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(s1000Engine, &params);
    akz_stretch_engine_set_source(s1000Engine, signal.data(), signal.size());
    size_t s1000Len = akz_stretch_engine_output_length(s1000Engine);
    std::vector<float> s1000Out(s1000Len);
    akz_stretch_engine_process(s1000Engine, s1000Out.data(), s1000Len);
    akz_stretch_engine_destroy(s1000Engine);

    params.machine = AkzMachine_S2000;
    AkzStretchEngine* s2000Engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(s2000Engine, &params);
    akz_stretch_engine_set_source(s2000Engine, signal.data(), signal.size());
    size_t s2000Len = akz_stretch_engine_output_length(s2000Engine);
    std::vector<float> s2000Out(s2000Len);
    akz_stretch_engine_process(s2000Engine, s2000Out.data(), s2000Len);
    akz_stretch_engine_destroy(s2000Engine);

    AKZ_CHECK_EQ(s1000Len, s2000Len); // same transpose ratio -> same length regardless of interpolator/filter kind
    bool anyDifference = false;
    for (size_t i = 0; i < s1000Out.size(); ++i) {
        if (std::fabs(s1000Out[i] - s2000Out[i]) > 1e-6f) { anyDifference = true; break; }
    }
    AKZ_CHECK(anyDifference);
}
