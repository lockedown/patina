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
