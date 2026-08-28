// ArtifactTests.cpp
//
// Stage 3's fidelity bar (plan section 8) says the engine is wrong if
// short cycle length doesn't ring metallic and long cycle length doesn't
// tremolo -- regardless of what the API-contract tests say. Both
// artifacts share one root cause (S950 manual, plan "2.4"): the block
// splice introduces periodicity at the cycle length. Short cycle -> that
// periodicity lands in the audio band (heard as pitch/ringing). Long
// cycle -> it lands below ~20 Hz (heard as tremolo). Same mechanism,
// different lag.
//
// That mechanism is directly measurable without needing ears: feed white
// noise (which has ~zero autocorrelation at any nonzero lag) through the
// engine and check the OUTPUT's autocorrelation at lag = cycleLength.
// White noise has no periodicity of its own, so any periodicity found at
// exactly the cycle length can only have been introduced by the splice --
// this is a direct measurement of the documented mechanism, not a proxy
// for it.

#include "TestFramework.h"
#include "include/AkaizerCore.h"

#include <cmath>
#include <vector>

namespace {

// Deterministic LCG noise generator -- reproducible across runs and
// platforms, unlike <random>'s engine-dependent output. Not
// cryptographic; just needs to look broadband to autocorrelation.
std::vector<float> makeWhiteNoise(size_t frameCount, uint32_t seed = 0x9E3779B9u) {
    std::vector<float> buf(frameCount);
    uint32_t state = seed;
    for (size_t i = 0; i < frameCount; ++i) {
        state = state * 1664525u + 1013904223u; // Numerical Recipes LCG
        // Map to [-1, 1).
        buf[i] = (static_cast<float>(state) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
    }
    return buf;
}

// Sum of two incommensurate low frequencies (~50 Hz and ~91 Hz @
// 44.1kHz) -- passes through a near-Nyquist-cutoff lowpass essentially
// untouched, while not repeating at any block spacing this file uses.
// See shortening_skips_whole_input_blocks_rather_than_resampling for why
// white noise doesn't work for that particular test once a real VCF is
// in the pipeline.
std::vector<float> makeLowFrequencySignal(size_t frameCount) {
    std::vector<float> buf(frameCount);
    for (size_t i = 0; i < frameCount; ++i) {
        buf[i] = 0.5f * std::sin(static_cast<float>(i) * 0.0071f) + 0.3f * std::sin(static_cast<float>(i) * 0.013f);
    }
    return buf;
}

// Normalised autocorrelation of `signal` at `lag` samples:
// sum(x[i] * x[i+lag]) / sum(x[i]^2), averaged over the signal with the
// tail (where i+lag would run off the end) excluded. 1.0 = perfectly
// self-similar at that lag, ~0 = no relationship (true for white noise
// at any nonzero lag), and it can go negative for anti-correlation.
double autocorrelationAtLag(const std::vector<float>& signal, size_t lag) {
    if (signal.size() <= lag) return 0.0;
    double numerator = 0.0;
    double denominator = 0.0;
    const size_t usable = signal.size() - lag;
    for (size_t i = 0; i < usable; ++i) {
        numerator += static_cast<double>(signal[i]) * static_cast<double>(signal[i + lag]);
    }
    for (size_t i = 0; i < signal.size(); ++i) {
        denominator += static_cast<double>(signal[i]) * static_cast<double>(signal[i]);
    }
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

AkzStretchParams makeParams(float timeFactorPercent, int cycleLength) {
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Classic;
    params.mode = AkzStretchMode_Cyclic;
    params.timeFactorPercent = timeFactorPercent;
    params.cycleLengthSamples = cycleLength;
    return params;
}

std::vector<float> renderAll(AkzStretchEngine* engine, const std::vector<float>& source, const AkzStretchParams& params) {
    akz_stretch_engine_reset(engine);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());

    size_t len = akz_stretch_engine_output_length(engine);
    std::vector<float> out(len);
    size_t written = akz_stretch_engine_process(engine, out.data(), len);
    out.resize(written);
    return out;
}

} // namespace

AKZ_TEST(white_noise_has_no_self_similarity_at_any_lag) {
    // Sanity check on the test signal itself: if this fails, the noise
    // generator is broken and the artifact tests below are meaningless.
    auto noise = makeWhiteNoise(20000);
    for (size_t lag : {50, 100, 500, 2000}) {
        double corr = autocorrelationAtLag(noise, lag);
        AKZ_CHECK(std::fabs(corr) < 0.05);
    }
}

AKZ_TEST(short_cycle_length_introduces_periodicity_at_cycle_lag_metallic_case) {
    // The metallic-ringing case (plan "2.4"): short cycle length, block
    // repeat rate lands in the audio band. S950 manual: "shorter D-TIME
    // values give rise to the metallic effect."
    auto noise = makeWhiteNoise(20000);
    const int cycleLength = 80; // 80 samples @ 44.1kHz ~ 551 Hz repeat rate -- audible

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto stretched = renderAll(engine, noise, makeParams(180.0f, cycleLength)); // lengthening -> blocks duplicated
    akz_stretch_engine_destroy(engine);

    double outputCorr = autocorrelationAtLag(stretched, static_cast<size_t>(cycleLength));
    double inputCorr = autocorrelationAtLag(noise, static_cast<size_t>(cycleLength));

    // The splice can only have introduced this -- the source had none.
    AKZ_CHECK(inputCorr < 0.05);
    AKZ_CHECK(outputCorr > 0.3);
}

AKZ_TEST(long_cycle_length_introduces_periodicity_at_cycle_lag_tremolo_case) {
    // The tremolo case (plan "2.4"): same mechanism, long cycle length,
    // so the repeat rate is sub-audio and reads as slow AM instead of
    // pitch. S950 manual: "Longer D-TIME values will give the sample a
    // slight tremolo effect."
    auto noise = makeWhiteNoise(40000);
    const int cycleLength = 4000; // 4000 samples @ 44.1kHz ~ 11 Hz repeat rate -- sub-audio, felt as tremolo

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto stretched = renderAll(engine, noise, makeParams(180.0f, cycleLength));
    akz_stretch_engine_destroy(engine);

    double outputCorr = autocorrelationAtLag(stretched, static_cast<size_t>(cycleLength));
    double inputCorr = autocorrelationAtLag(noise, static_cast<size_t>(cycleLength));

    AKZ_CHECK(inputCorr < 0.05);
    AKZ_CHECK(outputCorr > 0.3);
}

AKZ_TEST(shortening_skips_whole_input_blocks_rather_than_resampling) {
    // Compression's signature is different from lengthening's and white
    // noise autocorrelation can't reveal it: dropping DIFFERENT,
    // mutually-uncorrelated blocks of noise just produces more noise, no
    // periodicity -- discovered empirically while writing this test, and
    // correct, not a bug (duplicating noise creates real self-similarity;
    // skipping distinct chunks of it doesn't, whether or not a real
    // transient was in there to lose).
    //
    // What compression genuinely does, per the manual ("compressing a
    // sample removes certain bits of information"), is skip whole input
    // blocks rather than resampling through them. That's directly
    // checkable: with N=20000, cycle=80, ratio=0.5, numInBlocks=250 and
    // numOutBlocksClassic=round(20000*0.5/80)=125, so
    // inBlockIndex(k)=floor(k*250/125)=floor(2k) -- every EVEN input
    // block is used, every ODD one is skipped entirely. Block 1's exact
    // content should therefore appear nowhere in the output.
    //
    // Uses a smooth low-frequency signal rather than white noise here --
    // build order stage 6 added a real VCF as the last stage of the
    // pipeline (plan "4. Signal chain"), which is a genuine lowpass with
    // memory; applying any real lowpass to broadband noise inherently
    // changes its values (that's what a lowpass does), which would make
    // this test's exact-content comparison meaningless no matter the
    // tolerance. A signal built from two incommensurate low frequencies
    // stays essentially untouched by a filter opened up near Nyquist
    // (default filterCutoff01 == 1.0, per akz_stretch_params_default),
    // while still not repeating at the 80-sample block spacing this test
    // checks.
    auto noise = makeLowFrequencySignal(20000);
    const int cycleLength = 80;

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto compressed = renderAll(engine, noise, makeParams(50.0f, cycleLength));
    akz_stretch_engine_destroy(engine);

    AKZ_CHECK_EQ(compressed.size() % static_cast<size_t>(cycleLength), static_cast<size_t>(0));
    const size_t numOutBlocks = compressed.size() / static_cast<size_t>(cycleLength);

    // Only the head of each block is compared verbatim -- the tail is
    // deliberately crossfaded into the next mapped block (plan "2.1"),
    // so it won't equal raw source data even for a block the mapping
    // does use. cycleLength/2 stays safely clear of that crossfade
    // region without depending on the engine's exact overlap constant.
    const int compareLen = cycleLength / 2;

    // "Verbatim" is now "verbatim up to the source's own converter
    // quantisation and the VCF's steady-state response" (build order
    // stage 6 -- makeParams() defaults to S1000, 16-bit, and the filter
    // defaults fully open): the engine re-quantises _source fresh on
    // every recompute, and the VCF is a real IIR filter with memory, not
    // a true bypass at any cutoff -- even settled, it has a small but
    // real steady-state gain/phase effect (measured ~0.0002-0.003 on this
    // signal, well below its own ~0.5-0.8 amplitude). 0.01 comfortably
    // covers that while staying two orders of magnitude below the gap to
    // a genuinely different, unrelated block (checked below).
    const float quantiseTolerance = 0.01f;

    // input block 1 = noise[cycleLength .. 2*cycleLength)
    const float* skippedBlock = noise.data() + cycleLength;

    bool skippedBlockAppearsAtAnyOutputBoundary = false;
    for (size_t k = 0; k < numOutBlocks; ++k) {
        const float* outBlock = compressed.data() + k * static_cast<size_t>(cycleLength);
        bool matches = true;
        for (int i = 0; i < compareLen; ++i) {
            if (std::fabs(outBlock[i] - skippedBlock[i]) > quantiseTolerance) { matches = false; break; }
        }
        if (matches) { skippedBlockAppearsAtAnyOutputBoundary = true; break; }
    }
    AKZ_CHECK(!skippedBlockAppearsAtAnyOutputBoundary);

    // And an even-indexed input block -- one the mapping DOES use --
    // should appear verbatim (head region) at the corresponding output
    // block. Deliberately NOT output block 0: the VCF's state starts at
    // zero every render, so block 0 sits in the filter's startup
    // transient regardless of how smooth or in-passband the signal is --
    // that's a property of starting an IIR filter from silence, not of
    // the stretch algorithm this test is actually checking. Output block
    // 5 (-> input block 10, per inBlockIndex(k) = 2k) is 400 samples in,
    // comfortably past settling.
    const size_t checkOutBlock = 5;
    const size_t checkInBlock = 2 * checkOutBlock;
    AKZ_CHECK(checkOutBlock < numOutBlocks);
    bool laterBlockMatchesItsMappedInputBlock = true;
    for (int i = 0; i < compareLen; ++i) {
        const float outVal = compressed[checkOutBlock * static_cast<size_t>(cycleLength) + static_cast<size_t>(i)];
        const float srcVal = noise[checkInBlock * static_cast<size_t>(cycleLength) + static_cast<size_t>(i)];
        if (std::fabs(outVal - srcVal) > quantiseTolerance) { laterBlockMatchesItsMappedInputBlock = false; break; }
    }
    AKZ_CHECK(laterBlockMatchesItsMappedInputBlock);
}

AKZ_TEST(no_stretch_at_100_percent_stays_close_to_the_original) {
    // At exactly 100%, numOutBlocks == numInBlocks and every block maps
    // to itself (plan "2.1": inBlockIndex(k) = floor(k*numIn/numOut) = k
    // when numIn == numOut) -- CLASSIC should be a near-identity, not
    // introduce artifacts of its own. The crossfade blends two adjacent,
    // merely-correlated-by-proximity noise samples rather than two
    // spliced-together distant ones, so this should NOT show the strong
    // cycle-lag periodicity the artifact tests above deliberately
    // trigger -- confirming those tests are measuring the splice, not a
    // universal quirk of the crossfade math.
    auto noise = makeWhiteNoise(20000);
    const int cycleLength = 80;

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto passthrough = renderAll(engine, noise, makeParams(100.0f, cycleLength));
    akz_stretch_engine_destroy(engine);

    double outputCorr = autocorrelationAtLag(passthrough, static_cast<size_t>(cycleLength));
    AKZ_CHECK(outputCorr < 0.1);
}
