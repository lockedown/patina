// RealtimeChannelTests.cpp
//
// VstPlugin's reason for existing: RealtimeChannel is the block-streaming
// counterpart of applyRecordPath -> applyDacPath -> applyFilter, built so
// a live insert effect can run a machine's rate/converter/filter
// character on whatever audio the host hands it next, forever, with no
// whole-sample-known-in-advance assumption. These tests pin the two
// properties that make that safe:
//   - feeding the same audio through in different-sized host blocks must
//     be bit-identical (the whole point of moving state onto persistent
//     objects instead of resetting it every call);
//   - once settled, it must agree with the existing whole-buffer engine
//     (proof this is the same DSP, not a reimplementation that merely
//     resembles it).
// Plus safety/quality properties a live-automatable filter needs that a
// one-shot offline render never had to worry about: no discontinuity or
// non-finite output while cutoff/resonance sweep, and no click at a
// machine swap's crossfade boundary.

#include "TestFramework.h"
#include "../../Sources/Core/RealtimeChannel.h"
#include "../../Sources/Core/RateModel.h"
#include "../../Sources/Core/FilterModel.h"
#include "include/AkaizerCore.h"

#include <cmath>
#include <cstdint>
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

// Deterministic "noise" (no <random> dependency, matching this project's
// zero-dependency test stance) -- a cheap LCG is more than adequate for
// exercising a filter's stability across a wide spectrum.
std::vector<float> makeNoise(size_t count, uint32_t seed = 12345u) {
    std::vector<float> buf(count);
    uint32_t state = seed;
    for (size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        buf[i] = (static_cast<float>(state) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
    }
    return buf;
}

AkzRealtimeChannelParams makeParams(AkzMachine machine, int bitDepth, float sampleRateHz, float cutoff01, float resonance01) {
    AkzRealtimeChannelParams p;
    p.machine = machine;
    p.bitDepth = bitDepth;
    p.sampleRateHz = sampleRateHz;
    p.filterCutoff01 = cutoff01;
    p.filterResonance01 = resonance01;
    return p;
}

// Builds a channel already settled at `params` -- applies params, runs a
// throwaway block so the pending-params handoff and the first machine
// build take effect, then reset()s so cutoff/resonance smoothing starts
// AT the target instead of ramping from the constructor's own default.
// Callers still see a short filter-state transient after this (state
// itself is zeroed by reset()) -- same "skip the first ~500 samples"
// convention FilterModelTests.cpp uses for the whole-buffer engine.
std::unique_ptr<RealtimeChannel> makeSettledChannel(double hostSampleRateHz, size_t maxBlockFrames, const AkzRealtimeChannelParams& params) {
    auto channel = std::make_unique<RealtimeChannel>(hostSampleRateHz, maxBlockFrames);
    channel->setParams(params);
    std::vector<float> warmup(maxBlockFrames, 0.0f);
    channel->process(warmup.data(), warmup.size());
    channel->reset();
    return channel;
}

bool allFinite(const std::vector<float>& buf) {
    for (float v : buf) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

} // namespace

// -- Block-size invariance --------------------------------------------------

AKZ_TEST(realtime_channel_output_is_bit_identical_regardless_of_host_block_size) {
    const double hostRate = 44100.0;
    const AkzRealtimeChannelParams params = makeParams(AkzMachine_S3000, 0, 12000.0f, 0.6f, 0.4f);
    auto input = makeSine(6000, 220.0, hostRate);
    // A second tone mixed in so this isn't a single stationary frequency
    // -- a bug that only shows up on a changing signal (e.g. an off-by-
    // one in a phase carry) shouldn't hide behind a pure sine.
    auto sweep = makeNoise(6000, 999u);
    for (size_t i = 0; i < input.size(); ++i) input[i] = 0.7f * input[i] + 0.3f * sweep[i];

    const std::vector<size_t> blockSizes = {6000, 512, 441, 64, 1};

    std::vector<float> reference;
    for (size_t blockSize : blockSizes) {
        auto channel = makeSettledChannel(hostRate, 6000, params);
        auto buf = input;
        size_t offset = 0;
        while (offset < buf.size()) {
            const size_t chunk = std::min(blockSize, buf.size() - offset);
            channel->process(buf.data() + offset, chunk);
            offset += chunk;
        }
        if (reference.empty()) {
            reference = buf;
        } else {
            for (size_t i = 0; i < buf.size(); ++i) {
                AKZ_CHECK_EQ(buf[i], reference[i]);
            }
        }
    }
}

// -- Parity with the whole-buffer offline engine -----------------------------

AKZ_TEST(realtime_channel_matches_offline_record_dac_filter_chain_at_native_bit_depth) {
    const double hostRate = 44100.0;
    const AkzMachine machines[] = {AkzMachine_S950, AkzMachine_S1000, AkzMachine_S3000, AkzMachine_SP1200};

    for (AkzMachine machine : machines) {
        const AkzRealtimeChannelParams params = makeParams(machine, 0, 0.0f, 0.5f, 0.3f);
        auto input = makeSine(8000, 440.0, hostRate) ;
        auto noise = makeNoise(8000, 42u);
        for (size_t i = 0; i < input.size(); ++i) input[i] = 0.6f * input[i] + 0.2f * noise[i];

        // Reference: the exact call sequence RealtimeChannel's chain
        // mirrors, with transposeRatio fixed at 1.0 (no transpose in the
        // real-time path).
        auto reference = input;
        const double effectiveRateHz = resolveSampleRateHz(machine, params.sampleRateHz, hostRate);
        applyRecordPath(reference.data(), reference.size(), machine, effectiveRateHz, hostRate);
        applyDacPath(reference.data(), reference.size(), machine, effectiveRateHz, hostRate);
        applyFilter(reference.data(), reference.size(), machine, params.filterCutoff01, params.filterResonance01, hostRate, 1.0);

        auto channel = makeSettledChannel(hostRate, input.size(), params);
        auto actual = input;
        channel->process(actual.data(), actual.size());

        // Skip the startup transient: both paths begin every stateful
        // stage at zero, but RealtimeChannel's StreamingHold accumulates
        // its boundary phase slightly differently from holdAtRate's
        // whole-buffer version (see RateStages.h's header comment) --
        // same-recurrence, not same floating-point rounding history.
        const size_t skip = 1000;
        for (size_t i = skip; i < actual.size(); ++i) {
            AKZ_CHECK_NEAR(actual[i], reference[i], 5e-4);
        }
    }
}

// -- Retune continuity (no clicks, no NaN/Inf while automating) -------------

AKZ_TEST(cutoff_sweep_produces_no_discontinuity_and_stays_finite) {
    // A smooth low-frequency tone, not noise: white noise has huge
    // sample-to-sample deltas as its NORMAL character regardless of any
    // filter, which would swamp this test's whole point (catching a
    // retune-cadence artifact against an otherwise-smooth waveform).
    // Modest resonance for the same reason every_filter_topology_stays_
    // finite_at_maximum_resonance below is a separate, finiteness-only
    // test: near-self-oscillating resonance legitimately rings.
    const double hostRate = 44100.0;
    auto channel = makeSettledChannel(hostRate, 512, makeParams(AkzMachine_S3200, 0, 0.0f, 0.0f, 0.2f));
    auto signal = makeSine(44100, 220.0, hostRate, 0.8f);

    // Sweep cutoff from closed to open across the buffer, one control
    // update per block -- exactly the "drag a slider while audio plays"
    // scenario this whole real-time path exists for.
    const size_t blockSize = 256;
    float lastSample = 0.0f;
    bool first = true;
    for (size_t offset = 0; offset < signal.size(); offset += blockSize) {
        const size_t chunk = std::min(blockSize, signal.size() - offset);
        const float cutoff = static_cast<float>(offset) / static_cast<float>(signal.size());
        channel->setParams(makeParams(AkzMachine_S3200, 0, 0.0f, cutoff, 0.2f));
        channel->process(signal.data() + offset, chunk);

        for (size_t i = 0; i < chunk; ++i) {
            const float sample = signal[offset + i];
            AKZ_CHECK(std::isfinite(sample));
            if (!first) {
                AKZ_CHECK(std::fabs(sample - lastSample) < 0.5f); // no single-sample discontinuity
            }
            lastSample = sample;
            first = false;
        }
    }
}

AKZ_TEST(every_filter_topology_stays_finite_at_maximum_resonance) {
    const double hostRate = 44100.0;
    for (size_t i = 0; i < akz_machine_count(); ++i) {
        const AkzMachine machine = static_cast<AkzMachine>(i);
        if (akz_machine_profile(machine)->filterHasResonance == 0) continue;

        auto channel = makeSettledChannel(hostRate, 512, makeParams(machine, 0, 0.0f, 0.5f, 1.0f));
        auto signal = makeNoise(20000, 555u + static_cast<uint32_t>(i));
        channel->process(signal.data(), signal.size());
        AKZ_CHECK(allFinite(signal));
    }
}

// -- Bit-depth override -------------------------------------------------------

AKZ_TEST(bit_depth_override_of_zero_matches_machine_native_depth) {
    const double hostRate = 44100.0;
    const int nativeBits = akz_machine_profile(AkzMachine_S900)->bitDepth;

    auto input = makeSine(4000, 300.0, hostRate);

    auto withOverride = makeSettledChannel(hostRate, input.size(), makeParams(AkzMachine_S900, nativeBits, 0.0f, 1.0f, 0.0f));
    auto bufA = input;
    withOverride->process(bufA.data(), bufA.size());

    auto native = makeSettledChannel(hostRate, input.size(), makeParams(AkzMachine_S900, 0, 0.0f, 1.0f, 0.0f));
    auto bufB = input;
    native->process(bufB.data(), bufB.size());

    for (size_t i = 0; i < bufA.size(); ++i) {
        AKZ_CHECK_EQ(bufA[i], bufB[i]);
    }
}

AKZ_TEST(bit_depth_override_above_native_clamps_to_machine_native) {
    // The override is a crusher, not an upgrade: requesting 24 bits on
    // a 12-bit machine must resolve to the machine's own native depth,
    // bit-identical to passing 0 (native).
    const double hostRate = 44100.0;
    auto input = makeSine(4000, 300.0, hostRate);

    auto over = makeSettledChannel(hostRate, input.size(), makeParams(AkzMachine_S900, 24, 0.0f, 1.0f, 0.0f));
    auto bufA = input;
    over->process(bufA.data(), bufA.size());

    auto native = makeSettledChannel(hostRate, input.size(), makeParams(AkzMachine_S900, 0, 0.0f, 1.0f, 0.0f));
    auto bufB = input;
    native->process(bufB.data(), bufB.size());

    for (size_t i = 0; i < bufA.size(); ++i) {
        AKZ_CHECK_EQ(bufA[i], bufB[i]);
    }
}

AKZ_TEST(input_anti_alias_filter_runs_even_at_or_above_host_rate) {
    // The machine's input front end is always in circuit: at an
    // effective rate >= host rate the decimate/hold stages are
    // identity, but the AA filter still colours the signal -- output
    // must differ from a quantise-only reference.
    const double hostRate = 44100.0;
    auto input = makeSine(4000, 10000.0, hostRate); // high enough for the AA filter to bite

    auto channel = makeSettledChannel(hostRate, input.size(), makeParams(AkzMachine_S1000, 0, 0.0f, 1.0f, 0.0f));
    auto buf = input;
    channel->process(buf.data(), buf.size());

    auto quantiseOnly = input;
    quantizeBuffer(quantiseOnly.data(), quantiseOnly.size(), akz_machine_profile(AkzMachine_S1000)->bitDepth);

    bool differs = false;
    for (size_t i = 200; i < buf.size(); ++i) {
        if (std::fabs(buf[i] - quantiseOnly[i]) > 1e-4) { differs = true; break; }
    }
    AKZ_CHECK(differs);
}

AKZ_TEST(lower_bit_depth_override_produces_more_quantisation_error) {
    // Holds bypassed (sampleRateHz == hostRate) and filter left wide
    // open so bit-depth is the dominant source of the difference from
    // the raw input. The AA filter still runs (always in circuit) but
    // identically on both channels, so it cancels out of the
    // comparison.
    const double hostRate = 44100.0;
    auto input = makeSine(4000, 220.0, hostRate, 0.9f);

    auto low = makeSettledChannel(hostRate, input.size(), makeParams(AkzMachine_S1000, 4, static_cast<float>(hostRate), 1.0f, 0.0f));
    auto bufLow = input;
    low->process(bufLow.data(), bufLow.size());

    auto high = makeSettledChannel(hostRate, input.size(), makeParams(AkzMachine_S1000, 16, static_cast<float>(hostRate), 1.0f, 0.0f));
    auto bufHigh = input;
    high->process(bufHigh.data(), bufHigh.size());

    double errLow = 0.0, errHigh = 0.0;
    const size_t skip = 200;
    for (size_t i = skip; i < input.size(); ++i) {
        errLow += std::fabs(bufLow[i] - input[i]);
        errHigh += std::fabs(bufHigh[i] - input[i]);
    }
    AKZ_CHECK(errLow > errHigh);
}

// -- Machine swap crossfade ---------------------------------------------------

AKZ_TEST(machine_swap_crossfades_without_a_discontinuity) {
    const double hostRate = 44100.0;
    auto channel = makeSettledChannel(hostRate, 512, makeParams(AkzMachine_S1000, 0, 0.0f, 0.8f, 0.0f));
    auto signal = makeSine(20000, 220.0, hostRate, 0.8f);

    const size_t swapAt = 4000;
    const size_t blockSize = 256;
    float lastSample = 0.0f;
    bool first = true;
    for (size_t offset = 0; offset < signal.size(); offset += blockSize) {
        const size_t chunk = std::min(blockSize, signal.size() - offset);
        if (offset == swapAt) {
            channel->setParams(makeParams(AkzMachine_EmulatorII, 0, 0.0f, 0.8f, 0.0f));
        }
        channel->process(signal.data() + offset, chunk);
        for (size_t i = 0; i < chunk; ++i) {
            const float sample = signal[offset + i];
            AKZ_CHECK(std::isfinite(sample));
            if (!first) {
                AKZ_CHECK(std::fabs(sample - lastSample) < 0.5f);
            }
            lastSample = sample;
            first = false;
        }
    }
}

// -- reset() ------------------------------------------------------------------

AKZ_TEST(reset_clears_filter_memory_so_silence_stays_silent) {
    const double hostRate = 44100.0;
    auto channel = makeSettledChannel(hostRate, 512, makeParams(AkzMachine_S3200, 0, 0.0f, 0.3f, 1.0f));

    // Drive the resonant filter hard so it has real energy in its state.
    auto loud = makeNoise(4000, 21u);
    channel->process(loud.data(), loud.size());

    channel->reset();

    std::vector<float> silence(2000, 0.0f);
    channel->process(silence.data(), silence.size());

    const size_t skip = 50; // let any single-sample settling pass
    for (size_t i = skip; i < silence.size(); ++i) {
        AKZ_CHECK_NEAR(silence[i], 0.0f, 1e-4);
    }
}
