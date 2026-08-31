// FilterModelTests.cpp
//
// Correctness for build order stage 6's filter half -- see
// FilterModel.h. Calls applyFilter() directly rather than through the
// whole StretchEngine, to isolate the filter's own behaviour from the
// stretch/transpose stages already covered elsewhere.

#include "TestFramework.h"
#include "../../Sources/Core/FilterModel.h"
#include "include/AkaizerCore.h"

#include <cmath>
#include <vector>

using namespace akz;

namespace {

std::vector<float> makeSine(size_t count, double freqHz, double sampleRateHz, float amplitude = 0.8f) {
    std::vector<float> buf(count);
    for (size_t i = 0; i < count; ++i) {
        buf[i] = amplitude * static_cast<float>(std::sin(2.0 * M_PI * freqHz * static_cast<double>(i) / sampleRateHz));
    }
    return buf;
}

double rms(const std::vector<float>& buf, size_t skipFirst = 0) {
    double sumSq = 0.0;
    size_t n = 0;
    for (size_t i = skipFirst; i < buf.size(); ++i) {
        sumSq += static_cast<double>(buf[i]) * buf[i];
        ++n;
    }
    return n > 0 ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;
}

} // namespace

AKZ_TEST(closed_cutoff_attenuates_high_frequency_more_than_open_cutoff) {
    const double sampleRate = 44100.0;
    auto openBuf = makeSine(4000, 5000.0, sampleRate);
    auto closedBuf = openBuf;

    applyFilter(openBuf.data(), openBuf.size(), AkzMachine_S1000, 1.0f, 0.0f, sampleRate, 1.0);
    applyFilter(closedBuf.data(), closedBuf.size(), AkzMachine_S1000, 0.1f, 0.0f, sampleRate, 1.0);

    // Skip the first ~500 samples so the filter's startup transient
    // (state begins at zero -- see ArtifactTests.cpp's note on the same
    // issue) doesn't distort the steady-state RMS comparison.
    const double openRms = rms(openBuf, 500);
    const double closedRms = rms(closedBuf, 500);
    AKZ_CHECK(closedRms < openRms * 0.5); // meaningfully more attenuated, not just marginally
}

AKZ_TEST(pitch_tracking_filter_differs_across_transpose_ratios) {
    // S900/S950 track pitch (plan section 3.2 item 3) -- the same
    // nominal cutoff setting should produce a DIFFERENT actual cutoff,
    // and therefore different filtered output, depending on the
    // transpose ratio in effect.
    const double sampleRate = 44100.0;
    auto bufAtUnity = makeSine(4000, 5000.0, sampleRate);
    auto bufAtOctaveUp = bufAtUnity;

    applyFilter(bufAtUnity.data(), bufAtUnity.size(), AkzMachine_S950, 0.3f, 0.0f, sampleRate, 1.0);
    applyFilter(bufAtOctaveUp.data(), bufAtOctaveUp.size(), AkzMachine_S950, 0.3f, 0.0f, sampleRate, 2.0);

    bool anyDifference = false;
    for (size_t i = 500; i < bufAtUnity.size(); ++i) {
        if (std::fabs(bufAtUnity[i] - bufAtOctaveUp[i]) > 1e-6f) { anyDifference = true; break; }
    }
    AKZ_CHECK(anyDifference);
}

AKZ_TEST(non_tracking_filter_is_identical_across_transpose_ratios) {
    // S2000/S3000/S3200 run at a fixed cutoff regardless of pitch
    // ("runs at fixed 44.1kHz after pitch interpolation; cutoff does not
    // track pitch" -- plan section 3.4). Same setup as the tracking test
    // above, opposite expectation.
    const double sampleRate = 44100.0;
    auto bufAtUnity = makeSine(4000, 5000.0, sampleRate);
    auto bufAtOctaveUp = bufAtUnity;

    applyFilter(bufAtUnity.data(), bufAtUnity.size(), AkzMachine_S2000, 0.3f, 0.0f, sampleRate, 1.0);
    applyFilter(bufAtOctaveUp.data(), bufAtOctaveUp.size(), AkzMachine_S2000, 0.3f, 0.0f, sampleRate, 2.0);

    for (size_t i = 0; i < bufAtUnity.size(); ++i) {
        AKZ_CHECK_EQ(bufAtUnity[i], bufAtOctaveUp[i]);
    }
}

AKZ_TEST(svf_resonance_never_self_oscillates_even_at_maximum) {
    // "the code actually bottoms out at 1/16, so it never truly
    // self-oscillates" (plan section 3.4, citing the MAME device). Feed
    // an impulse into a resonant machine at maximum resonance and
    // confirm the response decays rather than growing without bound.
    const double sampleRate = 44100.0;
    std::vector<float> buf(10000, 0.0f);
    buf[0] = 1.0f; // impulse

    applyFilter(buf.data(), buf.size(), AkzMachine_S3000, 0.5f, 1.0f, sampleRate, 1.0);

    for (float v : buf) {
        AKZ_CHECK(!std::isnan(v));
        AKZ_CHECK(!std::isinf(v));
    }
    // The tail (well after the impulse) must have decayed close to
    // silence, not sustained a resonant ringing forever.
    double tailEnergy = rms(buf, 8000);
    AKZ_CHECK(tailEnergy < 0.05);
}

AKZ_TEST(resonance_changes_the_output) {
    const double sampleRate = 44100.0;
    auto noResonance = makeSine(4000, 3000.0, sampleRate);
    auto highResonance = noResonance;

    applyFilter(noResonance.data(), noResonance.size(), AkzMachine_S2000, 0.4f, 0.0f, sampleRate, 1.0);
    applyFilter(highResonance.data(), highResonance.size(), AkzMachine_S2000, 0.4f, 0.9f, sampleRate, 1.0);

    bool anyDifference = false;
    for (size_t i = 500; i < noResonance.size(); ++i) {
        if (std::fabs(noResonance[i] - highResonance[i]) > 1e-5f) { anyDifference = true; break; }
    }
    AKZ_CHECK(anyDifference);
}

AKZ_TEST(S3200_second_filter_stage_attenuates_more_than_S3000_single_stage) {
    // S3200 profile has filterSlopeDbPerOctave == 24 (the optional
    // second "2nd DIGITAL FILTER" in series -- plan section 3.4), so at
    // the same cutoff/resonance it should attenuate a high frequency
    // more than S3000's single 12 dB/oct stage.
    const double sampleRate = 44100.0;
    auto s3000Buf = makeSine(4000, 8000.0, sampleRate);
    auto s3200Buf = s3000Buf;

    applyFilter(s3000Buf.data(), s3000Buf.size(), AkzMachine_S3000, 0.3f, 0.0f, sampleRate, 1.0);
    applyFilter(s3200Buf.data(), s3200Buf.size(), AkzMachine_S3200, 0.3f, 0.0f, sampleRate, 1.0);

    const double s3000Rms = rms(s3000Buf, 500);
    const double s3200Rms = rms(s3200Buf, 500);
    AKZ_CHECK(s3200Rms < s3000Rms);
}

// -- v2 heritage-roster plan, "TPT SVF" stage: regression tests for the
// specific bug this migration fixes (ChamberlinSvf's k <= 1.1 clamp
// capping the real achievable cutoff around 8kHz and leaving an
// uncompensated resonant peak that could exceed 1.0 and clip
// downstream through PCMConversion.matchedGain). These would have
// FAILED against v1/pre-stage-6 ChamberlinSvf -- that's the point.

AKZ_TEST(resonant_machine_cutoff_reaches_above_the_old_8khz_ceiling) {
    const double sampleRate = 44100.0;
    // cutoff01 mapped log-20..Nyquist: a 12kHz-ish tone with cutoff set
    // well above it must pass through largely unattenuated -- impossible
    // under the old ChamberlinSvf clamp, whose real ceiling was ~8kHz
    // regardless of how open the cutoff control claimed to be.
    auto passed = makeSine(4000, 12000.0, sampleRate);
    auto reference = passed;
    applyFilter(passed.data(), passed.size(), AkzMachine_S3000, 0.95f, 0.0f, sampleRate, 1.0);

    const double passedRms = rms(passed, 500);
    const double referenceRms = rms(reference, 500);
    AKZ_CHECK(passedRms > referenceRms * 0.7); // meaningfully passed through, not still capped near 8kHz
}

AKZ_TEST(resonant_peak_never_exceeds_unity_at_full_compensation) {
    // filterResonanceCompensation01 == 1.0 for S2000/S3000/S3200 (full
    // compensation) -- the whole point of the fix. Sweep cutoff across
    // the range at maximum resonance and confirm the output peak never
    // exceeds the input peak, which the old uncompensated clamp could
    // not guarantee (the original bug report: an 0.8-amplitude 440Hz
    // tone came out peaking at 1.135).
    const double sampleRate = 44100.0;
    const float inputAmplitude = 0.8f;
    for (float cutoff01 = 0.1f; cutoff01 <= 1.0f; cutoff01 += 0.15f) {
        auto buf = makeSine(4000, 440.0, sampleRate, inputAmplitude);
        applyFilter(buf.data(), buf.size(), AkzMachine_S3000, cutoff01, 1.0f, sampleRate, 1.0);

        float peak = 0.0f;
        for (size_t i = 1000; i < buf.size(); ++i) { // skip startup transient
            peak = std::max(peak, std::fabs(buf[i]));
        }
        AKZ_CHECK(peak <= inputAmplitude * 1.05); // full compensation -- allow a hair of numerical slack, not a real margin
    }
}

AKZ_TEST(tpt_svf_stability_sweep_every_resonance_code_stays_finite) {
    // The sweep that caught ChamberlinSvf's k ~= 1.23 divergence,
    // repeated against TptSvf: all 16 resonance codes, 200k samples,
    // must never produce a non-finite value. TptSvf's zero-delay-
    // feedback structure is unconditionally stable by construction, so
    // this should trivially pass -- it exists to catch a REGRESSION
    // (e.g. a future edit reintroducing an unstable coefficient), not
    // because TptSvf is expected to be fragile the way ChamberlinSvf was.
    const double sampleRate = 44100.0;
    const size_t sampleCount = 200000;
    for (int resonanceCode = 0; resonanceCode <= 15; ++resonanceCode) {
        auto buf = makeSine(sampleCount, 1000.0, sampleRate, 0.9f);
        const float resonance01 = static_cast<float>(resonanceCode) / 15.0f;
        applyFilter(buf.data(), buf.size(), AkzMachine_S2000, 0.9f, resonance01, sampleRate, 1.0);
        for (float v : buf) {
            AKZ_CHECK(std::isfinite(v));
        }
    }
}

AKZ_TEST(tpt_svf_dc_gain_matches_the_documented_compensation_formula) {
    // The raw zero-delay-feedback SVF has unity DC gain for any k > 0 --
    // but filterResonanceCompensation01 == 1.0 (S2000/S3000/S3200)
    // deliberately scales the WHOLE signal path (input and output gain
    // are mathematically equivalent for a linear filter) by
    // 1 / (1 + c*(peakGain-1)), per FilterModel.cpp's own doc comment --
    // so full compensation trades away unity DC gain in exchange for
    // never clipping at the resonant peak, a real design tradeoff, not
    // a bug. At resonance01 == 0 there is no peak to compensate
    // (peakGain == 1), so compensation has zero effect regardless of the
    // profile's setting, and DC gain must be exactly unity.
    const double sampleRate = 44100.0;

    {
        std::vector<float> buf(4000, 0.5f);
        applyFilter(buf.data(), buf.size(), AkzMachine_S3000, 0.5f, 0.0f, sampleRate, 1.0);
        AKZ_CHECK_NEAR(buf.back(), 0.5, 1e-3);
    }
    {
        // resonanceCode 15 -> damping 1/16 -> k = 0.125 -> peakGain = 8
        // -> inputScale = 1/(1 + 1.0*(8-1)) = 0.125, matching
        // AkzMachineProfile.filterResonanceCompensation01 == 1.0 for
        // S3000 -- see MachineProfile.cpp.
        std::vector<float> buf(4000, 0.5f);
        applyFilter(buf.data(), buf.size(), AkzMachine_S3000, 0.5f, 1.0f, sampleRate, 1.0);
        AKZ_CHECK_NEAR(buf.back(), 0.5 * 0.125, 1e-3);
    }
}

// -- v2 heritage-roster plan, stage 10: SsmLadder and CemStateVariable,
// exercised through the real machines that use them ------------------

AKZ_TEST(ssm_ladder_stability_sweep_every_resonance_code_stays_finite) {
    // Same discipline as TptSvf's own sweep -- unlike TptSvf,
    // SsmLadder's feedback loop is NOT unconditionally stable by
    // construction (a real ladder self-oscillates, which a naive linear
    // model would let diverge to +-inf); this verifies the tanh
    // soft-clip in the feedback path actually bounds it, across the
    // full resonance range, over a run long enough for a slow
    // divergence to show -- the same class of bug the original
    // ChamberlinSVF k~=1.23 regression was.
    const double sampleRate = 44100.0;
    const size_t sampleCount = 200000;
    for (int resonanceCode = 0; resonanceCode <= 15; ++resonanceCode) {
        auto buf = makeSine(sampleCount, 1000.0, sampleRate, 0.9f);
        const float resonance01 = static_cast<float>(resonanceCode) / 15.0f;
        applyFilter(buf.data(), buf.size(), AkzMachine_EmulatorII, 0.9f, resonance01, sampleRate, 1.0);
        for (float v : buf) {
            AKZ_CHECK(std::isfinite(v));
        }
    }
}

AKZ_TEST(ssm_ladder_resonance_changes_the_output) {
    const double sampleRate = 44100.0;
    auto noResonance = makeSine(4000, 3000.0, sampleRate);
    auto highResonance = noResonance;

    applyFilter(noResonance.data(), noResonance.size(), AkzMachine_EmulatorII, 0.4f, 0.0f, sampleRate, 1.0);
    applyFilter(highResonance.data(), highResonance.size(), AkzMachine_EmulatorII, 0.4f, 0.9f, sampleRate, 1.0);

    bool anyDifference = false;
    for (size_t i = 500; i < noResonance.size(); ++i) {
        if (std::fabs(noResonance[i] - highResonance[i]) > 1e-5f) { anyDifference = true; break; }
    }
    AKZ_CHECK(anyDifference);
}

AKZ_TEST(ssm_ladder_never_exceeds_a_bounded_peak_even_at_maximum_resonance) {
    // The same passband-gain-compensation guarantee TptSvf's own
    // regression test checks, for the ladder topology: full
    // compensation (filterResonanceCompensation01 == 1.0 for the
    // Emulator II) must keep the output from running away, even while
    // genuinely self-oscillating at code 15.
    const double sampleRate = 44100.0;
    const float inputAmplitude = 0.8f;
    auto buf = makeSine(20000, 440.0, sampleRate, inputAmplitude);
    applyFilter(buf.data(), buf.size(), AkzMachine_EmulatorII, 0.6f, 1.0f, sampleRate, 1.0);

    float peak = 0.0f;
    for (size_t i = 2000; i < buf.size(); ++i) { // skip startup transient
        peak = std::max(peak, std::fabs(buf[i]));
    }
    AKZ_CHECK(peak < 10.0f); // bounded, not diverging -- not a tight loudness claim
}

AKZ_TEST(cem_state_variable_stability_sweep_every_resonance_code_stays_finite) {
    // CemStateVariable reuses TptSvf's math (unconditionally stable by
    // construction) -- this pins that guarantee at the machine level
    // too, for both machines that use it.
    const double sampleRate = 44100.0;
    const size_t sampleCount = 200000;
    const AkzMachine machines[] = {AkzMachine_FairlightCmi2x, AkzMachine_Mirage};
    for (AkzMachine machine : machines) {
        for (int resonanceCode = 0; resonanceCode <= 15; resonanceCode += 3) { // coarser step -- two machines x 16 codes x 200k would be slow for little extra confidence over TptSvf's own full sweep
            auto buf = makeSine(sampleCount, 1000.0, sampleRate, 0.9f);
            const float resonance01 = static_cast<float>(resonanceCode) / 15.0f;
            applyFilter(buf.data(), buf.size(), machine, 0.9f, resonance01, sampleRate, 1.0);
            for (float v : buf) {
                AKZ_CHECK(std::isfinite(v));
            }
        }
    }
}

AKZ_TEST(fairlight_and_mirage_second_filter_stage_attenuates_more_than_one_stage_would) {
    // Both use filterStageCount == 2 to reach their cited 24dB/oct --
    // same "second stage attenuates more" property S3200 already
    // proves against S3000's single stage, checked here against a
    // synthetic single-stage reference built from S2000 (also TptSvf-
    // family, one stage) at the same cutoff/resonance.
    const double sampleRate = 44100.0;
    auto oneStageBuf = makeSine(4000, 8000.0, sampleRate);
    auto twoStageBuf = oneStageBuf;

    applyFilter(oneStageBuf.data(), oneStageBuf.size(), AkzMachine_S2000, 0.3f, 0.0f, sampleRate, 1.0);
    applyFilter(twoStageBuf.data(), twoStageBuf.size(), AkzMachine_Mirage, 0.3f, 0.0f, sampleRate, 1.0);

    const double oneStageRms = rms(oneStageBuf, 500);
    const double twoStageRms = rms(twoStageBuf, 500);
    AKZ_CHECK(twoStageRms < oneStageRms);
}

AKZ_TEST(non_resonant_machines_have_no_resonance_parameter_effect) {
    // S900/S950/S1000 have no resonance control at all (plan section
    // 3.2 item 2) -- resonance01 must be silently ignored, not produce
    // some accidental effect via a stray code path.
    const double sampleRate = 44100.0;
    auto lowRes = makeSine(4000, 3000.0, sampleRate);
    auto highRes = lowRes;

    applyFilter(lowRes.data(), lowRes.size(), AkzMachine_S1000, 0.4f, 0.0f, sampleRate, 1.0);
    applyFilter(highRes.data(), highRes.size(), AkzMachine_S1000, 0.4f, 1.0f, sampleRate, 1.0);

    for (size_t i = 0; i < lowRes.size(); ++i) {
        AKZ_CHECK_EQ(lowRes[i], highRes[i]);
    }
}
