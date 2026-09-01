// RateModelTests.cpp
//
// v2 heritage-roster plan, stage 4. Calls resolveSampleRateHz() and
// applyRecordPath() directly rather than through the whole StretchEngine,
// same isolation principle as FilterModelTests.cpp.

#include "TestFramework.h"
#include "../../Sources/Core/RateModel.h"
#include "../../Sources/Core/ConverterModel.h"
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

// Same normalised-autocorrelation technique ArtifactTests.cpp uses to
// verify the stretch engine's own artifacts -- a separate copy here
// rather than a shared header, matching this project's existing
// per-test-file convention (FilterModelTests.cpp and ArtifactTests.cpp
// each have their own small measurement helpers too).
double autocorrelationAtLag(const std::vector<float>& signal, size_t lag) {
    if (lag >= signal.size()) return 0.0;
    double num = 0.0, denomA = 0.0, denomB = 0.0;
    for (size_t i = 0; i + lag < signal.size(); ++i) {
        num += static_cast<double>(signal[i]) * signal[i + lag];
        denomA += static_cast<double>(signal[i]) * signal[i];
        denomB += static_cast<double>(signal[i + lag]) * signal[i + lag];
    }
    const double denom = std::sqrt(denomA * denomB);
    return denom > 0.0 ? num / denom : 0.0;
}

} // namespace

AKZ_TEST(zero_sample_rate_resolves_to_machine_max_not_host_rate) {
    // 2.1 feedback: "never be 0 or bypassed as this is the essence of the
    // old sampler sound." Before this, <= 0 resolved to hostSampleRateHz
    // -- a true bypass -- and every preset/default decoded sampleRateHz
    // as exactly 0, so every machine shipped bypassed. Now 0 resolves to
    // the machine's own top-end rate (AkaizerCore.h's documented
    // contract for the field), so there is no value left that means
    // "skip the rate stage entirely."
    const AkzMachineProfile* s950 = akz_machine_profile(AkzMachine_S950);
    const AkzMachineProfile* s900 = akz_machine_profile(AkzMachine_S900);
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S950, 0.0f, 44100.0), s950->maxSampleRateHz, 0.001);
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S900, -5.0f, 44100.0), s900->maxSampleRateHz, 0.001);
}

AKZ_TEST(record_path_at_a_variable_machines_own_default_rate_is_not_a_no_op) {
    // Direct regression for the actual complaint, not just the resolver:
    // S900's own default (its maxSampleRateHz, 40000, resolved from the 0
    // sentinel above) is BELOW a 44.1kHz host, so running the record path
    // at that rate must genuinely decimate+anti-alias, not pass through
    // untouched the way record_path_at_host_rate_is_pure_quantisation_
    // no_incidental_filtering pins for the >= host-rate case.
    const double hostRate = 44100.0;
    const AkzMachineProfile* s900 = akz_machine_profile(AkzMachine_S900);
    auto source = makeSine(2000, 300.0, hostRate);
    auto viaRecordPath = source;
    auto viaQuantizeOnly = source;

    applyRecordPath(viaRecordPath.data(), viaRecordPath.size(), AkzMachine_S900, s900->maxSampleRateHz, hostRate);
    quantizeBuffer(viaQuantizeOnly.data(), viaQuantizeOnly.size(), s900->bitDepth);

    bool differsFromQuantiseOnly = false;
    for (size_t i = 0; i < source.size(); ++i) {
        if (std::abs(viaRecordPath[i] - viaQuantizeOnly[i]) > 1e-6) { differsFromQuantiseOnly = true; break; }
    }
    AKZ_CHECK(differsFromQuantiseOnly);
}

AKZ_TEST(positive_sample_rate_clamps_into_machine_range) {
    const AkzMachineProfile* s950 = akz_machine_profile(AkzMachine_S950);
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S950, 20000.0f, 44100.0), 20000.0, 0.001); // within range, passes through
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S950, 1.0f, 44100.0), s950->minSampleRateHz, 0.001); // below range, clamps up
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S950, 999999.0f, 44100.0), s950->maxSampleRateHz, 0.001); // above range, clamps down
}

AKZ_TEST(fixed_dual_rate_machine_collapses_any_request_to_its_two_real_rates) {
    // S1000 has hasVariableSampleRate == 0 -- min/max ARE its two real
    // selectable rates (22050/44100), not a continuous range. Clamping
    // into [min, max] must still resolve sensibly at both ends.
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S1000, 22050.0f, 44100.0), 22050.0, 0.001);
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S1000, 44100.0f, 44100.0), 44100.0, 0.001);
    AKZ_CHECK_NEAR(resolveSampleRateHz(AkzMachine_S1000, 1.0f, 44100.0), 22050.0, 0.001); // below range, clamps to the lower real rate
}

AKZ_TEST(record_path_at_host_rate_is_pure_quantisation_no_incidental_filtering) {
    // effectiveRateHz >= hostSampleRateHz must skip the anti-alias
    // filter entirely, not just skip decimation -- a machine at its own
    // native/host rate should sound identical to before this stage
    // existed. Verified by comparing against calling quantizeBuffer
    // directly with the same bit depth, bit for bit.
    auto source = makeSine(2000, 1000.0, 44100.0);
    auto viaRecordPath = source;
    auto viaQuantizeOnly = source;

    applyRecordPath(viaRecordPath.data(), viaRecordPath.size(), AkzMachine_S1000, 44100.0, 44100.0);
    quantizeBuffer(viaQuantizeOnly.data(), viaQuantizeOnly.size(), akz_machine_profile(AkzMachine_S1000)->bitDepth);

    for (size_t i = 0; i < source.size(); ++i) {
        AKZ_CHECK_NEAR(viaRecordPath[i], viaQuantizeOnly[i], 1e-9);
    }
}

AKZ_TEST(decimation_holds_each_sample_constant_across_a_machine_sample_period) {
    // The defining character of a coarse ADC: true decimation followed
    // by zero-order-hold reconstruction is a literal staircase. Assert
    // it directly rather than only by inference from a spectral test --
    // within each block of (hostRate / effectiveRate) samples, every
    // value must be bit-identical to the block's first sample.
    const double hostRate = 44100.0;
    const double effectiveRate = 10000.0; // S950 range, an arbitrary in-range value
    auto source = makeSine(4000, 300.0, hostRate); // low enough to pass the AA filter mostly intact

    applyRecordPath(source.data(), source.size(), AkzMachine_S950, effectiveRate, hostRate);

    const double samplesPerMachineSample = hostRate / effectiveRate;
    double nextBoundary = 0.0;
    float blockValue = source[0];
    for (size_t i = 0; i < source.size(); ++i) {
        if (static_cast<double>(i) >= nextBoundary) {
            blockValue = source[i];
            nextBoundary += samplesPerMachineSample;
        }
        AKZ_CHECK_EQ(source[i], blockValue);
    }
}

AKZ_TEST(decimated_output_is_quantised_to_the_machines_bit_depth) {
    // The record path's last step must still be real quantisation, not
    // just a rate effect layered on top of untouched float values.
    const double hostRate = 44100.0;
    const double effectiveRate = 8000.0;
    auto source = makeSine(4000, 200.0, hostRate);

    applyRecordPath(source.data(), source.size(), AkzMachine_S900, effectiveRate, hostRate);

    const int bitDepth = akz_machine_profile(AkzMachine_S900)->bitDepth;
    for (float sample : source) {
        float requantised = quantize(sample, bitDepth);
        AKZ_CHECK_NEAR(sample, requantised, 1e-6); // already quantised -> requantising is a no-op
    }
}

AKZ_TEST(decimating_below_a_tones_frequency_produces_the_predicted_alias) {
    // The spectral test: a tone ABOVE the new Nyquist must fold back down
    // to the classic alias frequency |f - round(f/fs)*fs|, not simply
    // disappear or pass through unchanged -- this IS the character
    // driving SP-1200/Emulator-style foldover (README's "Rate/Dac stage"
    // rationale), not a side effect to filter away.
    const double hostRate = 44100.0;
    const double effectiveRate = 10000.0; // new Nyquist = 5000 Hz
    const double toneFreq = 7000.0;       // above the new Nyquist
    // Standard aliasing formula: fold around the nearest multiple of the
    // sampling rate. round(7000/10000) = 1 -> alias at |7000-10000| = 3000 Hz.
    const double expectedAliasFreq = std::abs(toneFreq - std::round(toneFreq / effectiveRate) * effectiveRate);
    AKZ_CHECK_NEAR(expectedAliasFreq, 3000.0, 0.001); // sanity-check the arithmetic itself

    auto source = makeSine(8000, toneFreq, hostRate);
    applyRecordPath(source.data(), source.size(), AkzMachine_S950, effectiveRate, hostRate);

    const size_t aliasLagSamples = static_cast<size_t>(std::lround(hostRate / expectedAliasFreq));
    const size_t originalLagSamples = static_cast<size_t>(std::lround(hostRate / toneFreq));

    const double aliasCorr = autocorrelationAtLag(source, aliasLagSamples);
    const double originalCorr = autocorrelationAtLag(source, originalLagSamples);

    // The alias period must show up strongly; the original tone's own
    // period must not dominate (it was above Nyquist for the rate we
    // decimated to, so a coherent periodicity at the ORIGINAL frequency
    // can't survive the hold intact).
    AKZ_CHECK(aliasCorr > 0.5);
    AKZ_CHECK(aliasCorr > originalCorr);
}

// -- applyDacPath (v2 heritage-roster plan, stage 5) ------------------------

AKZ_TEST(dac_path_is_a_no_op_at_or_above_host_rate) {
    auto source = makeSine(2000, 1000.0, 44100.0);
    auto untouched = source;

    applyDacPath(source.data(), source.size(), AkzMachine_S900, 44100.0, 44100.0);
    for (size_t i = 0; i < source.size(); ++i) {
        AKZ_CHECK_EQ(source[i], untouched[i]);
    }
}

AKZ_TEST(dac_path_holds_each_sample_across_a_playback_clock_period) {
    // Same staircase character as the record path's decimation, but
    // driven by playbackRateHz (the caller's resolved DAC clock)
    // instead of the record-time effective rate -- the two legitimately
    // differ whenever transpose is in play on a pitch-tracking machine.
    const double hostRate = 44100.0;
    const double playbackRate = 12000.0;
    auto source = makeSine(4000, 300.0, hostRate);

    applyDacPath(source.data(), source.size(), AkzMachine_S900, playbackRate, hostRate);

    const double samplesPerPlaybackSample = hostRate / playbackRate;
    double nextBoundary = 0.0;
    float blockValue = source[0];
    for (size_t i = 0; i < source.size(); ++i) {
        if (static_cast<double>(i) >= nextBoundary) {
            blockValue = source[i];
            nextBoundary += samplesPerPlaybackSample;
        }
        AKZ_CHECK_EQ(source[i], blockValue);
    }
}

AKZ_TEST(dac_path_does_not_quantise_bit_depth_was_already_fixed_at_record_time) {
    // Re-quantising here would double-crush a signal whose bit depth was
    // already fixed by applyRecordPath -- feed a value that would NOT
    // survive 12-bit quantisation unchanged, and confirm applyDacPath
    // leaves it exactly as it found it (aside from the hold itself).
    const double hostRate = 44100.0;
    const double playbackRate = 20000.0;
    std::vector<float> source(2000, 0.123456789f); // a value quantize() would visibly move at 12 bits
    float beforeQuantize = source[0];
    float afterQuantizeWouldBe = quantize(beforeQuantize, akz_machine_profile(AkzMachine_S900)->bitDepth);
    AKZ_CHECK(std::abs(beforeQuantize - afterQuantizeWouldBe) > 1e-6); // sanity: quantising this value really would change it

    applyDacPath(source.data(), source.size(), AkzMachine_S900, playbackRate, hostRate);
    // Held blocks all derive from the constant input value, so every
    // sample must still be exactly the original, un-quantised value.
    for (float sample : source) {
        AKZ_CHECK_NEAR(sample, beforeQuantize, 1e-9);
    }
}

AKZ_TEST(record_path_never_changes_buffer_length_only_content) {
    // Length-neutrality is the one hard invariant every caller depends
    // on (StretchEngine::outputLength()'s mirrored arithmetic, the
    // realtime player's two-phase publish protocol) -- applyRecordPath
    // takes a raw buffer + count rather than a vector specifically so
    // there is no length to accidentally change; this pins that down by
    // checking every element is still finite and the count passed in is
    // the count actually touched (no out-of-bounds write, checked via a
    // sentinel-guarded oversized allocation).
    const size_t count = 1000;
    std::vector<float> buf(count + 2, -999.0f); // sentinels front/back
    auto sine = makeSine(count, 440.0, 44100.0);
    for (size_t i = 0; i < count; ++i) buf[i + 1] = sine[i];

    applyRecordPath(buf.data() + 1, count, AkzMachine_S900, 12000.0, 44100.0);

    AKZ_CHECK_EQ(buf.front(), -999.0f); // untouched
    AKZ_CHECK_EQ(buf.back(), -999.0f);  // untouched
    for (size_t i = 1; i <= count; ++i) {
        AKZ_CHECK(std::isfinite(buf[i]));
    }
}
