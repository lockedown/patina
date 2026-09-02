// IntelligentModeTests.cpp
//
// Correctness for build order stage 7's INTELLIGENT mode (plan "2.2") --
// the SOLA-style splice-point search implemented in
// StretchEngine::_synthesizeIntelligent. CYCLIC mode's own correctness
// is already covered by StretchEngineTests.cpp and ArtifactTests.cpp;
// this file is specific to what's new here: the search itself, the
// quality/width parameters that only apply in this mode, and the
// S950 "no mode switch" exception.

#include "TestFramework.h"
#include "include/AkaizerCore.h"

#include <cmath>
#include <vector>

namespace {

std::vector<float> makeSine(size_t count, double freqHz, double sampleRateHz, float amplitude = 0.7f) {
    std::vector<float> buf(count);
    for (size_t i = 0; i < count; ++i) {
        buf[i] = amplitude * static_cast<float>(std::sin(2.0 * M_PI * freqHz * static_cast<double>(i) / sampleRateHz));
    }
    return buf;
}

AkzStretchParams makeIntelligentParams(AkzMachine machine, float timeFactorPercent, int quality, int width) {
    AkzStretchParams params;
    akz_stretch_params_default(machine, &params);
    params.mode = AkzStretchMode_Intelligent;
    params.timeFactorPercent = timeFactorPercent;
    params.quality = quality;
    params.width = width;
    return params;
}

} // namespace

AKZ_TEST(intelligent_lengthening_produces_longer_sane_output) {
    auto signal = makeSine(20000, 220.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S1000, 180.0f, 50, 50);

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, signal.data(), signal.size());

    size_t len = akz_stretch_engine_output_length(engine);
    AKZ_CHECK(len > signal.size());

    std::vector<float> out(len);
    size_t written = akz_stretch_engine_process(engine, out.data(), len);
    AKZ_CHECK_EQ(written, len);
    for (float v : out) {
        AKZ_CHECK(!std::isnan(v));
        AKZ_CHECK(!std::isinf(v));
    }
    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(intelligent_shortening_produces_shorter_sane_output) {
    auto signal = makeSine(20000, 220.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S3000, 60.0f, 50, 50);

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, signal.data(), signal.size());

    size_t len = akz_stretch_engine_output_length(engine);
    AKZ_CHECK(len < signal.size());
    AKZ_CHECK(len > 0);

    std::vector<float> out(len);
    akz_stretch_engine_process(engine, out.data(), len);
    for (float v : out) {
        AKZ_CHECK(!std::isnan(v));
        AKZ_CHECK(!std::isinf(v));
    }
    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(intelligent_output_length_matches_prediction_classic) {
    // The same invariant StretchEngineTests.cpp checks for CYCLIC:
    // outputLength() (queryable before process()) must exactly match
    // what process() actually produces -- otherwise the app can't size
    // buffers correctly, and the offline/real-time bit-identity
    // guarantee (plan "2.5") would only hold by accident.
    auto signal = makeSine(15000, 330.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S2000, 143.0f, 30, 70);

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, signal.data(), signal.size());

    size_t predicted = akz_stretch_engine_output_length(engine);
    std::vector<float> out(predicted + 100, -999.0f); // pad so a length mismatch is obvious, not silently truncated
    size_t written = akz_stretch_engine_process(engine, out.data(), out.size());

    AKZ_CHECK_EQ(written, predicted);
    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(intelligent_output_length_matches_prediction_revised) {
    auto signal = makeSine(15000, 330.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S2000, 143.0f, 30, 70);
    params.engine = AkzEngine_Revised;

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, signal.data(), signal.size());

    size_t predicted = akz_stretch_engine_output_length(engine);
    // REVISED promises the exact requested length regardless of mode --
    // plan "2.3", now generalised beyond just CYCLIC.
    size_t expectedExact = static_cast<size_t>(std::llround(signal.size() * 1.43));
    AKZ_CHECK_EQ(predicted, expectedExact);

    std::vector<float> out(predicted);
    size_t written = akz_stretch_engine_process(engine, out.data(), predicted);
    AKZ_CHECK_EQ(written, predicted);
    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(intelligent_streaming_in_small_chunks_matches_one_big_offline_call) {
    // Same test as StretchEngineTests.cpp's CYCLIC version -- proves
    // real-time audition (which pulls in small chunks) and offline
    // rendering agree bit-for-bit under INTELLIGENT too, not just CYCLIC.
    auto signal = makeSine(12000, 440.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S3000, 165.0f, 40, 40);

    AkzStretchEngine* offlineEngine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(offlineEngine, &params);
    akz_stretch_engine_set_source(offlineEngine, signal.data(), signal.size());
    size_t totalLen = akz_stretch_engine_output_length(offlineEngine);
    std::vector<float> offlineOut(totalLen);
    akz_stretch_engine_process(offlineEngine, offlineOut.data(), totalLen);
    akz_stretch_engine_destroy(offlineEngine);

    AkzStretchEngine* streamEngine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(streamEngine, &params);
    akz_stretch_engine_set_source(streamEngine, signal.data(), signal.size());
    std::vector<float> streamOut;
    streamOut.reserve(totalLen);
    float chunk[97]; // not a divisor of anything -- exercises arbitrary chunk boundaries
    size_t got;
    while ((got = akz_stretch_engine_process(streamEngine, chunk, 97)) > 0) {
        streamOut.insert(streamOut.end(), chunk, chunk + got);
    }
    akz_stretch_engine_destroy(streamEngine);

    AKZ_CHECK_EQ(streamOut.size(), offlineOut.size());
    bool identical = true;
    for (size_t i = 0; i < streamOut.size() && i < offlineOut.size(); ++i) {
        if (streamOut[i] != offlineOut[i]) { identical = false; break; }
    }
    AKZ_CHECK(identical);
}

AKZ_TEST(S950_ignores_intelligent_mode_request) {
    // S950 has no mode switch (Mon1/Pol2 instead -- plan section 3.2);
    // AkzStretchParams.mode must be ignored for it, per that field's own
    // doc comment in AkaizerCore.h.
    auto signal = makeSine(8000, 220.0, 44100.0);

    AkzStretchParams intelligentRequest = makeIntelligentParams(AkzMachine_S950, 150.0f, 50, 50);
    AkzStretchParams explicitCyclic = intelligentRequest;
    explicitCyclic.mode = AkzStretchMode_Cyclic;

    AkzStretchEngine* engineA = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engineA, &intelligentRequest);
    akz_stretch_engine_set_source(engineA, signal.data(), signal.size());
    size_t lenA = akz_stretch_engine_output_length(engineA);
    std::vector<float> outA(lenA);
    akz_stretch_engine_process(engineA, outA.data(), lenA);
    akz_stretch_engine_destroy(engineA);

    AkzStretchEngine* engineB = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engineB, &explicitCyclic);
    akz_stretch_engine_set_source(engineB, signal.data(), signal.size());
    size_t lenB = akz_stretch_engine_output_length(engineB);
    std::vector<float> outB(lenB);
    akz_stretch_engine_process(engineB, outB.data(), lenB);
    akz_stretch_engine_destroy(engineB);

    AKZ_CHECK_EQ(lenA, lenB);
    bool identical = (lenA == lenB);
    if (identical) {
        for (size_t i = 0; i < outA.size(); ++i) {
            if (outA[i] != outB[i]) { identical = false; break; }
        }
    }
    AKZ_CHECK(identical);
}

AKZ_TEST(width_parameter_changes_the_output_length_in_classic) {
    // width controls the crossfade region, which determines the
    // synthesis hop (frameSize - overlap) and therefore the natural
    // (CLASSIC, non-corrected) output length -- see _planIntelligent.
    // Confirms width is actually wired through, not accepted and ignored.
    auto signal = makeSine(30000, 300.0, 44100.0);

    AkzStretchParams narrowWidth = makeIntelligentParams(AkzMachine_S1000, 150.0f, 50, 0);
    AkzStretchParams wideWidth = makeIntelligentParams(AkzMachine_S1000, 150.0f, 50, 99);

    AkzStretchEngine* engineA = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engineA, &narrowWidth);
    akz_stretch_engine_set_source(engineA, signal.data(), signal.size());
    size_t lenNarrow = akz_stretch_engine_output_length(engineA);
    akz_stretch_engine_destroy(engineA);

    AkzStretchEngine* engineB = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engineB, &wideWidth);
    akz_stretch_engine_set_source(engineB, signal.data(), signal.size());
    size_t lenWide = akz_stretch_engine_output_length(engineB);
    akz_stretch_engine_destroy(engineB);

    AKZ_CHECK(lenNarrow != lenWide);
}

AKZ_TEST(quality_search_actually_changes_which_material_gets_spliced_in) {
    // quality=0 means searchRange=0 (see _intelligentSearchRange): the
    // ONLY candidate considered is the nominal position, so the splice
    // is deterministic and content-blind. quality=99 searches nearby
    // offsets and picks whichever correlates best with what's already
    // been written -- "intelligently varies the interpolation rate
    // according to the sample content" (plan "2.2"). This test doesn't
    // try to prove that choice is objectively "better" (a smooth sine
    // plus wide overlap-add smoothing turned out to make that
    // surprisingly hard to measure cleanly -- the crossfade itself
    // spreads any discontinuity over hundreds of samples, and a
    // multi-splice buffer's own worst natural slope dominates a
    // single-jump metric regardless of alignment quality). What's
    // directly verifiable is that the search is real and wired through:
    // given a signal with content for it to react to and room to search
    // (width=0 keeps the splice a hard edit, maximally sensitive to
    // which offset gets chosen), quality=0 and quality=99 must select
    // different material at at least one splice.
    const double sampleRate = 44100.0;
    // Two incommensurate frequencies -- like ArtifactTests.cpp's
    // low-frequency signal, but higher and less smooth than a single
    // sine, so different splice offsets are very likely to land on
    // measurably different sample values rather than by-coincidence
    // near-identical ones.
    std::vector<float> signal(20000);
    for (size_t i = 0; i < signal.size(); ++i) {
        signal[i] = 0.5f * std::sin(static_cast<float>(i) * 0.31f) + 0.3f * std::sin(static_cast<float>(i) * 0.17f);
    }

    AkzStretchParams noSearch = makeIntelligentParams(AkzMachine_S1000, 130.0f, 0, 0);
    AkzStretchParams fullSearch = noSearch;
    fullSearch.quality = 99;

    auto render = [&](const AkzStretchParams& params) {
        AkzStretchEngine* engine = akz_stretch_engine_create(sampleRate);
        AkzStretchParams p = params;
        akz_stretch_engine_set_params(engine, &p);
        akz_stretch_engine_set_source(engine, signal.data(), signal.size());
        size_t len = akz_stretch_engine_output_length(engine);
        std::vector<float> out(len);
        akz_stretch_engine_process(engine, out.data(), len);
        akz_stretch_engine_destroy(engine);
        return out;
    };

    auto outNoSearch = render(noSearch);
    auto outFullSearch = render(fullSearch);

    AKZ_CHECK_EQ(outNoSearch.size(), outFullSearch.size());
    bool anyDifference = false;
    for (size_t i = 0; i < outNoSearch.size() && i < outFullSearch.size(); ++i) {
        if (outNoSearch[i] != outFullSearch[i]) { anyDifference = true; break; }
    }
    AKZ_CHECK(anyDifference);
}

// -- 2.1 stereo splice linkage ------------------------------------------
//
// "Still splitting channels and phasing when in realtime edit mode" --
// each channel's own StretchEngine picks its SOLA splice offset by
// cross-correlating ITS OWN content, so two different channels (L/R of
// one stereo file) can and do choose different offsets at the same
// nominal position: stereo decorrelation. setSpliceGuide is what a
// coordinator (ProcessedRender.swift / LiveAuditionController.swift)
// uses to force every channel's engine to splice at the SAME positions
// instead, derived from one shared (typically mid/summed) analysis pass.

AKZ_TEST(without_a_guide_two_different_signals_choose_different_splice_offsets) {
    // Establishes the bug this fixes, not just the fix: two genuinely
    // different signals, same length/params, independently searching
    // their own content, land on different offsets somewhere in a long
    // enough render. If this ever stopped being true the regression test
    // below would be vacuous.
    auto signalA = makeSine(20000, 220.0, 44100.0);
    auto signalB = makeSine(20000, 830.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S1000, 150.0f, 60, 50);

    AkzStretchEngine* engineA = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engineA, &params);
    akz_stretch_engine_set_source(engineA, signalA.data(), signalA.size());
    std::vector<float> outA(akz_stretch_engine_output_length(engineA));
    akz_stretch_engine_process(engineA, outA.data(), outA.size());

    AkzStretchEngine* engineB = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engineB, &params);
    akz_stretch_engine_set_source(engineB, signalB.data(), signalB.size());
    std::vector<float> outB(akz_stretch_engine_output_length(engineB));
    akz_stretch_engine_process(engineB, outB.data(), outB.size());

    size_t countA = akz_stretch_engine_last_splice_offset_count(engineA);
    size_t countB = akz_stretch_engine_last_splice_offset_count(engineB);
    AKZ_CHECK(countA > 0);
    AKZ_CHECK_EQ(countA, countB); // purely length/params-derived -- see _planIntelligent

    std::vector<long long> offsetsA(countA), offsetsB(countB);
    akz_stretch_engine_get_last_splice_offsets(engineA, offsetsA.data(), countA);
    akz_stretch_engine_get_last_splice_offsets(engineB, offsetsB.data(), countB);

    bool anyOffsetDiffers = false;
    for (size_t i = 0; i < countA; ++i) {
        if (offsetsA[i] != offsetsB[i]) { anyOffsetDiffers = true; break; }
    }
    AKZ_CHECK(anyOffsetDiffers);

    akz_stretch_engine_destroy(engineA);
    akz_stretch_engine_destroy(engineB);
}

AKZ_TEST(same_guide_makes_two_different_signals_choose_identical_splice_offsets) {
    // The actual fix, direct: same two signals as above, but both given
    // the SAME guide (here, derived from A's own unguided search --
    // standing in for a real mid/summed analysis pass, which is exactly
    // as "not this engine's own content" from B's point of view).
    auto signalA = makeSine(20000, 220.0, 44100.0);
    auto signalB = makeSine(20000, 830.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S1000, 150.0f, 60, 50);

    AkzStretchEngine* guideSource = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(guideSource, &params);
    akz_stretch_engine_set_source(guideSource, signalA.data(), signalA.size());
    std::vector<float> guideRender(akz_stretch_engine_output_length(guideSource));
    akz_stretch_engine_process(guideSource, guideRender.data(), guideRender.size());
    size_t guideCount = akz_stretch_engine_last_splice_offset_count(guideSource);
    AKZ_CHECK(guideCount > 0);
    std::vector<long long> guide(guideCount);
    akz_stretch_engine_get_last_splice_offsets(guideSource, guide.data(), guideCount);
    akz_stretch_engine_destroy(guideSource);

    auto renderGuided = [&](const std::vector<float>& signal) {
        AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
        akz_stretch_engine_set_splice_guide(engine, guide.data(), guide.size());
        akz_stretch_engine_set_params(engine, &params);
        akz_stretch_engine_set_source(engine, signal.data(), signal.size());
        std::vector<float> out(akz_stretch_engine_output_length(engine));
        akz_stretch_engine_process(engine, out.data(), out.size());
        size_t count = akz_stretch_engine_last_splice_offset_count(engine);
        std::vector<long long> usedOffsets(count);
        akz_stretch_engine_get_last_splice_offsets(engine, usedOffsets.data(), count);
        akz_stretch_engine_destroy(engine);
        return usedOffsets;
    };

    auto usedByA = renderGuided(signalA);
    auto usedByB = renderGuided(signalB);

    // Both channels used the guide verbatim, not their own search --
    // identical to each other AND identical to the guide itself.
    AKZ_CHECK_EQ(usedByA.size(), guide.size());
    AKZ_CHECK_EQ(usedByB.size(), guide.size());
    for (size_t i = 0; i < guide.size(); ++i) {
        AKZ_CHECK_EQ(usedByA[i], guide[i]);
        AKZ_CHECK_EQ(usedByB[i], guide[i]);
    }
}

AKZ_TEST(clearing_the_guide_reverts_to_independent_search) {
    auto signal = makeSine(20000, 220.0, 44100.0);
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S1000, 150.0f, 60, 50);

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    // An all-zero guide is trivially distinguishable from a real search's
    // offsets on this signal/params combination (verified below).
    std::vector<long long> zeroGuide(64, 0);
    akz_stretch_engine_set_splice_guide(engine, zeroGuide.data(), zeroGuide.size());
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, signal.data(), signal.size());
    std::vector<float> guidedOut(akz_stretch_engine_output_length(engine));
    akz_stretch_engine_process(engine, guidedOut.data(), guidedOut.size());
    size_t guidedCount = akz_stretch_engine_last_splice_offset_count(engine);
    std::vector<long long> guidedOffsets(guidedCount);
    akz_stretch_engine_get_last_splice_offsets(engine, guidedOffsets.data(), guidedCount);

    akz_stretch_engine_clear_splice_guide(engine);
    akz_stretch_engine_reset(engine); // force a genuinely fresh recompute, not the cheap filter-only path
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, signal.data(), signal.size());
    std::vector<float> searchedOut(akz_stretch_engine_output_length(engine));
    akz_stretch_engine_process(engine, searchedOut.data(), searchedOut.size());
    size_t searchedCount = akz_stretch_engine_last_splice_offset_count(engine);
    std::vector<long long> searchedOffsets(searchedCount);
    akz_stretch_engine_get_last_splice_offsets(engine, searchedOffsets.data(), searchedCount);

    AKZ_CHECK_EQ(guidedCount, searchedCount);
    bool anyOffsetDiffers = false;
    for (size_t i = 0; i < guidedCount && i < zeroGuide.size(); ++i) {
        if (guidedOffsets[i] != searchedOffsets[i]) { anyOffsetDiffers = true; break; }
    }
    AKZ_CHECK(anyOffsetDiffers);

    akz_stretch_engine_destroy(engine);
}

AKZ_TEST(intelligent_handles_source_shorter_than_one_frame) {
    // Falls back to a plain resample (see _planIntelligent's useFallback
    // branch) rather than crashing or producing garbage when there isn't
    // enough signal for frame-based analysis to mean anything.
    auto tinySignal = makeSine(20, 1000.0, 44100.0); // far shorter than the ~30ms (~1323 sample) frame size
    AkzStretchParams params = makeIntelligentParams(AkzMachine_S1000, 150.0f, 50, 50);

    AkzStretchEngine* engine = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(engine, &params);
    akz_stretch_engine_set_source(engine, tinySignal.data(), tinySignal.size());

    size_t len = akz_stretch_engine_output_length(engine);
    AKZ_CHECK(len > 0);
    std::vector<float> out(len);
    size_t written = akz_stretch_engine_process(engine, out.data(), len);
    AKZ_CHECK_EQ(written, len);
    for (float v : out) {
        AKZ_CHECK(!std::isnan(v));
        AKZ_CHECK(!std::isinf(v));
    }
    akz_stretch_engine_destroy(engine);
}
