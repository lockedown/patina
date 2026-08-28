// RealtimeStretchPlayerTests.cpp
//
// Two different concerns, tested separately:
//   1. Correctness -- once a render has published, pulled audio actually
//      matches what AkzStretchEngine would produce for the same source
//      and params (i.e. the background recompute isn't doing anything
//      different from the synchronous path), and it loops correctly.
//   2. Thread safety -- pull() (simulating the render thread) survives
//      concurrent setSource()/setParams() calls (simulating the UI
//      thread dragging sliders) without crashing, hanging, or producing
//      NaN/garbage. This can't prove the absence of every possible race,
//      but it exercises the exact concurrent-access pattern the app
//      actually has for real, repeatedly, under real thread scheduling.

#include "TestFramework.h"
#include "include/AkaizerCore.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {

std::vector<float> makeRamp(size_t frameCount) {
    std::vector<float> buf(frameCount);
    for (size_t i = 0; i < frameCount; ++i) {
        buf[i] = static_cast<float>(i) / static_cast<float>(frameCount);
    }
    return buf;
}

// Every render -- including a fresh player's very first one -- is
// stretch-affecting (there's nothing to reapply a filter onto yet), so
// it publishes to the PENDING slot, not directly, and needs an explicit
// commitPending() to become visible to pull()/is_ready(). Real callers
// gate that commit across every channel (see LiveAuditionController's
// render callback); a lone test player has no siblings to wait for, so
// committing the moment it has something pending is the correct
// single-channel analogue, not a shortcut around the new contract.
void waitUntilReady(AkzRealtimePlayer* player, int maxMillis = 2000) {
    int waited = 0;
    while (!akz_realtime_player_is_ready(player) && waited < maxMillis) {
        if (akz_realtime_player_has_pending_commit(player)) {
            akz_realtime_player_commit_pending(player);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        waited += 2;
    }
}

void waitForPendingCommit(AkzRealtimePlayer* player, int maxMillis = 2000) {
    int waited = 0;
    while (!akz_realtime_player_has_pending_commit(player) && waited < maxMillis) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        waited += 2;
    }
}

} // namespace

AKZ_TEST(realtime_player_starts_not_ready_and_produces_silence) {
    AkzRealtimePlayer* player = akz_realtime_player_create(44100.0);
    AKZ_CHECK(!akz_realtime_player_is_ready(player));

    float buf[128];
    size_t written = akz_realtime_player_pull(player, buf, 128);
    AKZ_CHECK_EQ(written, static_cast<size_t>(128)); // always fills the request -- with silence if nothing published yet
    for (float f : buf) {
        AKZ_CHECK(f == 0.0f);
    }

    akz_realtime_player_destroy(player);
}

AKZ_TEST(realtime_player_becomes_ready_and_matches_offline_engine) {
    auto source = makeRamp(4000);

    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.timeFactorPercent = 150.0f;
    params.cycleLengthSamples = 200;

    // Reference: what the synchronous, already-tested offline engine
    // produces for the same input.
    AkzStretchEngine* offline = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(offline, &params);
    akz_stretch_engine_set_source(offline, source.data(), source.size());
    size_t refLen = akz_stretch_engine_output_length(offline);
    std::vector<float> reference(refLen);
    akz_stretch_engine_process(offline, reference.data(), refLen);
    akz_stretch_engine_destroy(offline);

    AkzRealtimePlayer* player = akz_realtime_player_create(44100.0);
    akz_realtime_player_set_source(player, source.data(), source.size());
    akz_realtime_player_set_params(player, &params);
    waitUntilReady(player);
    AKZ_CHECK(akz_realtime_player_is_ready(player));

    // Pull exactly one loop's worth and compare -- the background
    // recompute must produce the identical render the offline path does.
    std::vector<float> pulled(refLen);
    akz_realtime_player_pull(player, pulled.data(), refLen);

    bool identical = true;
    for (size_t i = 0; i < refLen; ++i) {
        if (pulled[i] != reference[i]) { identical = false; break; }
    }
    AKZ_CHECK(identical);

    akz_realtime_player_destroy(player);
}

AKZ_TEST(realtime_player_loops_seamlessly_past_the_end) {
    auto source = makeRamp(2000);
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S950, &params);
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = 500;

    AkzRealtimePlayer* player = akz_realtime_player_create(44100.0);
    akz_realtime_player_set_source(player, source.data(), source.size());
    akz_realtime_player_set_params(player, &params);
    waitUntilReady(player);

    // Pull well past one loop's length in small chunks; must never
    // return short (silence padding) once ready, and must wrap around
    // rather than running off the end.
    float chunk[97]; // deliberately not a divisor of any buffer length
    size_t totalPulled = 0;
    for (int i = 0; i < 500; ++i) {
        size_t got = akz_realtime_player_pull(player, chunk, 97);
        AKZ_CHECK_EQ(got, static_cast<size_t>(97));
        totalPulled += got;
        for (float f : chunk) {
            AKZ_CHECK(!std::isnan(f));
            AKZ_CHECK(!std::isinf(f));
        }
    }
    AKZ_CHECK(totalPulled > source.size() * 3); // definitely looped multiple times

    akz_realtime_player_destroy(player);
}

AKZ_TEST(realtime_player_filter_only_change_does_not_reset_read_position) {
    // Regression test for the "restarts the sample on every slide
    // movement, especially filter and resonance" report -- see the plan.
    // A filter-only setParams() must take RealtimeStretchPlayer's cheap
    // reapplyFilterOnly() path, which cannot change buffer length, and
    // must leave the read position exactly where it was.
    auto source = makeRamp(2000);
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Classic;
    params.mode = AkzStretchMode_Cyclic;
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = 200; // divides 2000 evenly -> output length == source length
    params.filterCutoff01 = 1.0f;

    AkzRealtimePlayer* player = akz_realtime_player_create(44100.0);
    akz_realtime_player_set_source(player, source.data(), source.size());
    akz_realtime_player_set_params(player, &params);
    waitUntilReady(player);
    AKZ_CHECK(akz_realtime_player_is_ready(player));

    // Advance the read position partway through the loop (25%), well
    // clear of both the start and the one-pole filter's brief startup
    // transient, before making a filter-only change.
    float discard[500];
    size_t got = akz_realtime_player_pull(player, discard, 500);
    AKZ_CHECK_EQ(got, static_cast<size_t>(500));

    // Filter-only change: everything else identical, only cutoff moves.
    params.filterCutoff01 = 0.5f;
    akz_realtime_player_set_params(player, &params);
    // No completion signal is exposed for "this specific recompute
    // finished" (is_ready() only ever means "something has published, at
    // some point") -- a fixed, generous sleep stands in, well beyond what
    // a background thread needs to filter a 2000-sample buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    float one[1];
    akz_realtime_player_pull(player, one, 1);
    // A reset-to-position-0 (the bug) would read back near the ramp's
    // very start (~0.0 either filtered or not); the correct, continuous
    // position is near source[500]/2000 == 0.25 -- a one-pole filter this
    // close to identity can't blur that distinction away.
    AKZ_CHECK(one[0] > 0.15f);
    AKZ_CHECK(one[0] < 0.35f);

    akz_realtime_player_destroy(player);
}

AKZ_TEST(realtime_player_stretch_affecting_change_publishes_pending_then_resets_on_commit) {
    // Companion to the filter-only test above: a change that DOES need a
    // full re-render (here, cycleLengthSamples -- stretch-affecting, not
    // filter-only) must NOT reset the read position (or become visible
    // to pull() at all) the moment the background worker finishes --
    // only once commitPending() is explicitly called.
    //
    // This used to publish and reset-to-0 unconditionally the instant
    // the worker finished. That was itself a fix for an even earlier bug
    // (proportional remapping desynchronising channels permanently -- see
    // git blame), but it carried a subtler bug of its own:
    // LiveAuditionController runs one independent RealtimeStretchPlayer
    // PER CHANNEL, each with its own background worker racing the
    // others. Publishing unilaterally meant whichever channel's worker
    // finished first would start playing the new buffer from 0 while a
    // sibling channel -- still finishing the SAME change -- kept playing
    // the OLD buffer for however long that gap lasted: heard as
    // artificial stereo width on every stretch-affecting knob (not just
    // Cycle, and not just once -- continuously, for as long as knob
    // dragging keeps re-triggering the gap). Deferring the publish to a
    // PENDING slot until an explicit, externally-gated commitPending()
    // call is what lets LiveAuditionController's render callback commit
    // every channel together instead. See
    // two_independent_players_commit_gated_together_stay_in_sync below
    // for the direct multi-channel version of this.
    auto source = makeRamp(2000);
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Classic;
    params.mode = AkzStretchMode_Cyclic;
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = 200; // -> output length 2000 (divides evenly)

    // Reference for the OLD params (cycleLengthSamples == 200), computed
    // before the change below -- the pipeline (quantize + filter) means
    // this is close to but not bit-identical to the raw source, so this,
    // not source itself, is what "still serving the old buffer" has to
    // match.
    AkzStretchEngine* oldOffline = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(oldOffline, &params);
    akz_stretch_engine_set_source(oldOffline, source.data(), source.size());
    size_t oldLen = akz_stretch_engine_output_length(oldOffline);
    std::vector<float> oldReference(oldLen);
    akz_stretch_engine_process(oldOffline, oldReference.data(), oldLen);
    akz_stretch_engine_destroy(oldOffline);

    AkzRealtimePlayer* player = akz_realtime_player_create(44100.0);
    akz_realtime_player_set_source(player, source.data(), source.size());
    akz_realtime_player_set_params(player, &params);
    waitUntilReady(player);

    // Advance to 25% through the (length-2000) loop, well clear of 0.
    float discard[500];
    akz_realtime_player_pull(player, discard, 500);

    // Stretch-affecting change: cycleLengthSamples 200 -> 300.
    params.cycleLengthSamples = 300;
    akz_realtime_player_set_params(player, &params);
    waitForPendingCommit(player);
    AKZ_CHECK(akz_realtime_player_has_pending_commit(player));

    // Not committed yet -- pull() must still be serving the OLD buffer,
    // continuing on from where it left off (position 500), not the new
    // one and not position 0.
    float stillOld[1];
    akz_realtime_player_pull(player, stillOld, 1);
    AKZ_CHECK_EQ(stillOld[0], oldReference[500]);

    // Reference: an independent offline engine given the exact same
    // (new) params and source -- the same equivalence this file's
    // "matches_offline_engine" test already establishes for the initial
    // render also holds for a live re-render mid-stream.
    AkzStretchEngine* offline = akz_stretch_engine_create(44100.0);
    akz_stretch_engine_set_params(offline, &params);
    akz_stretch_engine_set_source(offline, source.data(), source.size());
    size_t refLen = akz_stretch_engine_output_length(offline);
    std::vector<float> reference(refLen);
    akz_stretch_engine_process(offline, reference.data(), refLen);
    akz_stretch_engine_destroy(offline);

    akz_realtime_player_commit_pending(player);
    AKZ_CHECK(!akz_realtime_player_has_pending_commit(player)); // consumed

    float one[1];
    akz_realtime_player_pull(player, one, 1);
    AKZ_CHECK_EQ(one[0], reference[0]);

    akz_realtime_player_destroy(player);
}

AKZ_TEST(two_independent_players_commit_gated_together_stay_in_sync) {
    // Direct regression test for the actual reported scenario, rather
    // than just the single-instance mechanism above: LiveAuditionController
    // (LiveAuditionController.swift) owns one independent
    // RealtimeStretchPlayer per channel, each with its own worker
    // thread, and its render callback only calls commitPending() on
    // every channel once ALL of them report hasPendingCommit() true --
    // simulated here exactly, with a real stagger between the two
    // setParams() calls standing in for two channels' independent
    // worker threads being scheduled at different wall-clock times.
    auto source = makeRamp(2000);
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Classic;
    params.mode = AkzStretchMode_Cyclic;
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = 200;

    AkzRealtimePlayer* left = akz_realtime_player_create(44100.0);
    AkzRealtimePlayer* right = akz_realtime_player_create(44100.0);
    akz_realtime_player_set_source(left, source.data(), source.size());
    akz_realtime_player_set_source(right, source.data(), source.size());
    akz_realtime_player_set_params(left, &params);
    akz_realtime_player_set_params(right, &params);

    // The render callback's gate, in miniature: wait until every channel
    // has a pending commit, then commit them all in one go.
    auto gateCommitBoth = [&] {
        int waited = 0;
        while ((!akz_realtime_player_has_pending_commit(left) || !akz_realtime_player_has_pending_commit(right))
               && waited < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            waited += 2;
        }
        AKZ_CHECK(akz_realtime_player_has_pending_commit(left));
        AKZ_CHECK(akz_realtime_player_has_pending_commit(right));
        akz_realtime_player_commit_pending(left);
        akz_realtime_player_commit_pending(right);
    };
    gateCommitBoth(); // a fresh player's first render is stretch-affecting too

    // Advance both channels identically, exactly as the same render
    // callback pulling the same frameCount from every channel would.
    float discard[500];
    akz_realtime_player_pull(left, discard, 500);
    akz_realtime_player_pull(right, discard, 500);

    // The stretch-affecting change, staggered: left's worker gets it and
    // 100ms head start before right's even arrives -- the exact
    // thread-scheduling skew the bug report was about.
    params.cycleLengthSamples = 300;
    akz_realtime_player_set_params(left, &params);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // While only LEFT has a pending commit, the gate must refuse to
    // commit either one -- this is the actual mechanism of the fix, not
    // just its eventual outcome. Prove it directly: pull from both now,
    // with nothing committed, and they must still be identical, still on
    // the OLD buffer.
    AKZ_CHECK(akz_realtime_player_has_pending_commit(left));
    AKZ_CHECK(!akz_realtime_player_has_pending_commit(right)); // hasn't even been asked yet
    float leftMid[32];
    float rightMid[32];
    akz_realtime_player_pull(left, leftMid, 32);
    akz_realtime_player_pull(right, rightMid, 32);
    bool stillInSyncDuringGap = true;
    for (int i = 0; i < 32; ++i) {
        if (leftMid[i] != rightMid[i]) { stillInSyncDuringGap = false; break; }
    }
    AKZ_CHECK(stillInSyncDuringGap);

    akz_realtime_player_set_params(right, &params);
    gateCommitBoth();

    // Both channels must land on the exact same sample once committed
    // together, every step of the way -- not just once, but
    // continuously, as live audition actually plays.
    float leftBuf[64];
    float rightBuf[64];
    akz_realtime_player_pull(left, leftBuf, 64);
    akz_realtime_player_pull(right, rightBuf, 64);
    bool inSync = true;
    for (int i = 0; i < 64; ++i) {
        if (leftBuf[i] != rightBuf[i]) { inSync = false; break; }
    }
    AKZ_CHECK(inSync);

    akz_realtime_player_destroy(left);
    akz_realtime_player_destroy(right);
}

AKZ_TEST(realtime_player_survives_concurrent_param_changes_while_pulling) {
    // Simulates the app's actual concurrency pattern: one thread pulls
    // continuously (the render thread) while another thread hammers
    // setSource()/setParams() (the UI thread dragging sliders). No
    // assertion about audio content here -- this is purely "does it
    // survive," which is exactly what the render-thread-safety promise
    // in AkaizerCore.h is about.
    AkzRealtimePlayer* player = akz_realtime_player_create(44100.0);

    std::atomic<bool> stop{false};
    std::atomic<bool> sawNaNOrInf{false};

    std::thread pullThread([&] {
        std::vector<float> buf(512);
        while (!stop.load()) {
            akz_realtime_player_pull(player, buf.data(), buf.size());
            for (float f : buf) {
                if (std::isnan(f) || std::isinf(f)) {
                    sawNaNOrInf.store(true);
                }
            }
        }
    });

    std::thread paramThread([&] {
        for (int i = 0; i < 200; ++i) {
            auto source = makeRamp(1000 + static_cast<size_t>(i % 50) * 37);
            akz_realtime_player_set_source(player, source.data(), source.size());

            AkzStretchParams params;
            akz_stretch_params_default(AkzMachine_S1000, &params);
            params.timeFactorPercent = 50.0f + static_cast<float>(i % 20) * 10.0f;
            params.cycleLengthSamples = 50 + (i % 10) * 40;
            akz_realtime_player_set_params(player, &params);

            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    paramThread.join();
    waitUntilReady(player, 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // let the pull thread run a bit more with a ready buffer
    stop.store(true);
    pullThread.join();

    AKZ_CHECK(akz_realtime_player_is_ready(player));
    AKZ_CHECK(!sawNaNOrInf.load());

    akz_realtime_player_destroy(player);
}
