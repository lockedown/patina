// StretchEngineTests.cpp
//
// Correctness tests for the stretch engine's API contract -- length
// arithmetic, streaming/offline equivalence, parameter gating. This file
// deliberately does NOT try to assert "sounds metallic" or "sounds like
// tremolo" -- that's the by-ear fidelity bar in plan section 8, not
// something a unit test can meaningfully check.

#include "TestFramework.h"
#include "include/AkaizerCore.h"
#include "../../Sources/Core/ConverterModel.h"
#include "../../Sources/Core/FilterModel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

std::vector<float> makeSineSource(size_t frameCount, double cyclesPerBuffer = 37.0) {
    std::vector<float> buf(frameCount);
    for (size_t i = 0; i < frameCount; ++i) {
        double phase = 2.0 * M_PI * cyclesPerBuffer * static_cast<double>(i) / static_cast<double>(frameCount);
        buf[i] = static_cast<float>(std::sin(phase));
    }
    return buf;
}

} // namespace

// Regression test for the crossfade time-alignment bug fixed in
// _synthesizeCyclicBlocks: when cycleLength divides the source length
// exactly, CLASSIC/CYCLIC at timeFactorPercent == 100 maps every output
// block to itself (inBlockIndex(k) == k, plan "2.1"), so the crossfade's
// two legs read the exact same samples and the whole stretch stage
// collapses to a true identity on the quantised source. Before the fix,
// leg `b` read `overlap` samples into the future of leg `a` even in this
// contiguous case, so the crossfade region diverged from an identity --
// this is exactly what let CYCLIC audibly buzz at 100% with nothing
// asked of it (S950's "distorted" report -- see the plan). The engine
// always applies a real (near-identity but non-trivial) filter after the
// stretch stage, so the reference here is built by applying that SAME
// filter call directly to the quantised source, isolating the stretch
// stage rather than expecting the whole pipeline to be a no-op.
//
// "Identity" here means within float32 rounding, not literal bit
// equality: even when a == b mathematically (both legs read the exact
// same source sample once time-aligned), the crossfade still computes
// a*(1-t) + b*t rather than special-casing that equality away, and two
// separate float multiplies plus an add can round to a value one ULP
// off from a bare copy. Measured max deviation is ~6e-8 (one float32
// ULP at this magnitude) over 41 of 4000 samples -- all inside the
// crossfade region, none outside it. 1e-5 is comfortably above that and
// far below anything a real bug (the old ~-3 dB comb) would produce.
AKZ_TEST(cyclic_at_100_percent_with_dividing_cycle_length_is_bit_exact_identity) {
    auto source = makeSineSource(4000, 23.0);
    const int cycleLength = 200; // divides 4000 evenly -> 20 whole blocks, no remainder

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Classic;
    params.mode = AkzStretchMode_Cyclic;
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = cycleLength;

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());
    size_t len = akz_stretch_engine_output_length(engine);
    std::vector<float> actual(len);
    size_t written = akz_stretch_engine_process(engine, actual.data(), len);
    actual.resize(written);
    akz_stretch_engine_destroy(engine);

    AKZ_CHECK_EQ(actual.size(), source.size());

    // transposeSemitones defaults to 0 -> transposeRatio == 1.0 exactly,
    // and the engine skips its resample-for-transpose step entirely at
    // 0 semitones (StretchEngine.cpp), so this filter call with a fixed
    // ratio of 1.0 is exactly what the engine itself runs.
    std::vector<float> reference = source;
    akz::quantizeBuffer(reference.data(), reference.size(), 16); // S1000 bit depth
    akz::applyFilter(reference.data(), reference.size(), AkzMachine_S1000, params.filterCutoff01, params.filterResonance01, 44100.0, 1.0);

    AKZ_CHECK_EQ(actual.size(), reference.size());
    double maxDiff = 0.0;
    for (size_t i = 0; i < actual.size(); ++i) {
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(actual[i] - reference[i])));
    }
    AKZ_CHECK(maxDiff < 1e-5);
}

AKZ_TEST(classic_output_length_is_quantised_to_whole_cycles) {
    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto source = makeSineSource(8000);

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Classic;
    params.timeFactorPercent = 150.0f; // 1.5x
    params.cycleLengthSamples = 200;

    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());

    size_t len = akz_stretch_engine_output_length(engine);
    // round(8000 * 1.5 / 200) * 200 = round(60) * 200 = 12000
    AKZ_CHECK_EQ(len, static_cast<size_t>(12000));
    AKZ_CHECK(len % 200 == 0); // the defining property of CLASSIC's quantisation

    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(revised_output_length_is_exact_not_quantised) {
    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto source = makeSineSource(8000);

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Revised;
    params.timeFactorPercent = 137.0f; // deliberately not a clean multiple of the cycle
    params.cycleLengthSamples = 200;

    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());

    size_t len = akz_stretch_engine_output_length(engine);
    // round(8000 * 1.37) = round(10960) = 10960 exactly -- REVISED promises
    // exact timing (plan "2.3"), unlike CLASSIC above.
    AKZ_CHECK_EQ(len, static_cast<size_t>(10960));

    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(streaming_in_small_chunks_matches_one_big_offline_call) {
    // The single most important test in this file: real-time audition and
    // offline rendering must be bit-identical (plan "2.5 Streaming").
    // Simulated here by pulling process() in small chunks vs one large
    // chunk and comparing sample-for-sample.
    auto source = makeSineSource(5000);

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S950, &params);
    params.timeFactorPercent = 180.0f;
    params.cycleLengthSamples = 150;

    AkzStretchEngine* offlineEngine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(offlineEngine, &params);
    akz_stretch_engine_set_source(offlineEngine, source.data(), source.size());
    size_t totalLen = akz_stretch_engine_output_length(offlineEngine);

    std::vector<float> offlineOut(totalLen);
    size_t offlineWritten = akz_stretch_engine_process(offlineEngine, offlineOut.data(), totalLen);
    AKZ_CHECK_EQ(offlineWritten, totalLen);

    AkzStretchEngine* streamEngine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(streamEngine, &params);
    akz_stretch_engine_set_source(streamEngine, source.data(), source.size());

    std::vector<float> streamOut;
    streamOut.reserve(totalLen);
    float chunk[64];
    size_t got;
    while ((got = akz_stretch_engine_process(streamEngine, chunk, 64)) > 0) {
        streamOut.insert(streamOut.end(), chunk, chunk + got);
    }

    AKZ_CHECK_EQ(streamOut.size(), offlineOut.size());
    bool identical = true;
    for (size_t i = 0; i < streamOut.size() && i < offlineOut.size(); ++i) {
        if (streamOut[i] != offlineOut[i]) { identical = false; break; }
    }
    AKZ_CHECK(identical);

    akz_stretch_engine_destroy(offlineEngine);
    akz_stretch_engine_destroy(streamEngine);
}

AKZ_TEST(process_returns_zero_once_source_exhausted) {
    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto source = makeSineSource(1000);

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S950, &params);
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = 100;
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());

    size_t len = akz_stretch_engine_output_length(engine);
    std::vector<float> buf(len);
    size_t written = akz_stretch_engine_process(engine, buf.data(), len);
    AKZ_CHECK_EQ(written, len);

    float extra[16];
    size_t moreWritten = akz_stretch_engine_process(engine, extra, 16);
    AKZ_CHECK_EQ(moreWritten, static_cast<size_t>(0));

    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(shortening_below_100_percent_produces_shorter_output) {
    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto source = makeSineSource(8000);

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S3000, &params);
    params.engine = AkzEngine_Classic;
    params.timeFactorPercent = 50.0f;
    params.cycleLengthSamples = 200;
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());

    size_t len = akz_stretch_engine_output_length(engine);
    AKZ_CHECK_EQ(len, static_cast<size_t>(4000)); // round(8000*0.5/200)*200

    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(reset_clears_source_and_output) {
    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    auto source = makeSineSource(4000);

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S950, &params);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, source.data(), source.size());
    AKZ_CHECK(akz_stretch_engine_output_length(engine) > 0);

    akz_stretch_engine_reset(engine);
    AKZ_CHECK_EQ(akz_stretch_engine_output_length(engine), static_cast<size_t>(0));

    akz_stretch_engine_destroy(engine);
}
