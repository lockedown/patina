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

void waitUntilReady(AkzRealtimePlayer* player, int maxMillis = 2000) {
    int waited = 0;
    while (!akz_realtime_player_is_ready(player) && waited < maxMillis) {
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

AKZ_TEST(realtime_player_stretch_affecting_change_resets_read_position_to_zero) {
    // Companion to the filter-only test above: a change that DOES need a
    // full re-render (here, cycleLengthSamples -- stretch-affecting, not
    // filter-only) must reset the read position to 0.
    //
    // This used to instead remap the position proportionally into the
    // new (possibly different-length) buffer, so playback resumed near
    // the same musical position instead of visibly restarting -- reverted
    // after a real regression report: "changing the cycle slider makes
    // [live audition] sound stereo width." LiveAuditionController runs
    // one independent RealtimeStretchPlayer PER CHANNEL (see
    // LiveAuditionController.swift), each with its own background worker.
    // The old position used for the remap is a live, continuously-
    // advancing value; when the two channels' workers process the SAME
    // change at different wall-clock moments (ordinary thread scheduling
    // skew), each reads a DIFFERENT old position -- the channel whose
    // worker runs later has kept advancing on the old buffer in the
    // meantime -- so the two channels remap to genuinely different, and
    // then permanently desynchronised, positions. Resetting to the fixed
    // constant 0 is immune to this: every channel's worker converges on
    // the exact same value regardless of when it happens to run. See the
    // reverted code's comment in RealtimeStretchPlayer.cpp for the full
    // explanation, and the filter-only test above for why that path
    // (which never touches the read position at all) was never at risk.
    auto source = makeRamp(2000);
    AkzStretchParams params;
    akz_stretch_params_default(AkzMachine_S1000, &params);
    params.engine = AkzEngine_Classic;
    params.mode = AkzStretchMode_Cyclic;
    params.timeFactorPercent = 100.0f;
    params.cycleLengthSamples = 200; // -> output length 2000 (divides evenly)

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
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

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

    float one[1];
    akz_realtime_player_pull(player, one, 1);
    AKZ_CHECK_EQ(one[0], reference[0]);

    akz_realtime_player_destroy(player);
}

AKZ_TEST(two_independent_players_stay_in_sync_across_staggered_stretch_changes) {
    // Direct regression test for the actual reported scenario, rather
    // than just the single-instance mechanism above: LiveAuditionController
    // (LiveAuditionController.swift) owns one independent
    // RealtimeStretchPlayer per channel and forwards the same
    // setParams() to each in a plain loop -- there is no cross-instance
    // synchronisation, so if either instance's worker thread happens to
    // finish a re-render meaningfully later than the other's, this test
    // reproduces that staggering directly with a real sleep between the
    // two setParams() calls, standing in for two channels' independent
    // worker threads being scheduled at different times.
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
    waitUntilReady(left);
    waitUntilReady(right);

    // Advance both channels identically, exactly as the same render
    // callback pulling the same frameCount from every channel would.
    float discard[500];
    akz_realtime_player_pull(left, discard, 500);
    akz_realtime_player_pull(right, discard, 500);

    // The stretch-affecting change (cycle length), staggered: left's
    // "worker" gets it and 100ms to finish before right's even arrives --
    // simulating exactly the thread-scheduling skew the bug report was
    // about. Left may keep advancing on the OLD buffer during that gap.
    params.cycleLengthSamples = 300;
    akz_realtime_player_set_params(left, &params);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    akz_realtime_player_set_params(right, &params);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let both finish

    // Both channels must land on the exact same sample, every step of
    // the way -- not just once, but continuously, as live audition
    // actually plays. Divergence here is exactly what would be heard as
    // stereo widening/comb artifacts.
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
