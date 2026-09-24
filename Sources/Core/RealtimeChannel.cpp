// RealtimeChannel.cpp
//
// See RealtimeChannel.h for the design rationale. This file also holds
// the akz_realtime_channel_* C API functions, same convention as
// StretchEngine.cpp's akz_stretch_engine_* bottom section.

#include "RealtimeChannel.h"
#include "RateModel.h"

#include <algorithm>
#include <cmath>

namespace akz {

namespace {

// Control-rate tick length for filter retune and cutoff/resonance
// smoothing -- see .h's header comment. Small enough that a fast knob
// sweep still sounds continuous, large enough that retuning (a handful
// of sin/cos/tan/exp calls per filter stage) isn't paid every sample for
// no audible benefit.
constexpr size_t kControlIntervalFrames = 32;

// One-pole smoothing coefficient for cutoff/resonance -- ~20ms time
// constant, independent of host sample rate (recomputed from
// hostSampleRateHz at construction). Fast enough that a knob move feels
// immediate, slow enough that a single control-rate tick's coefficient
// jump (see kControlIntervalFrames) never produces an audible step.
double smoothingCoefficientFor(double hostSampleRateHz) {
    constexpr double kTimeConstantSeconds = 0.02;
    return 1.0 - std::exp(-1.0 / (kTimeConstantSeconds * hostSampleRateHz));
}

// ~10ms equal-power crossfade window for a machine swap -- see .h's
// header comment on why only a machine change (not cutoff/resonance/
// bandwidth/bit-depth) needs this at all.
size_t crossfadeLengthFor(double hostSampleRateHz) {
    return std::max<size_t>(1, static_cast<size_t>(hostSampleRateHz * 0.01));
}

int resonanceCodeFromSmoothed(float resonance01) {
    const float clamped = std::max(0.0f, std::min(1.0f, resonance01));
    return static_cast<int>(std::lround(clamped * 15.0f));
}

} // namespace

RealtimeChannel::MachineChain::MachineChain(AkzMachine m, double hostRate)
    : machine(m), profile(machineProfile(m)), hostSampleRateHz(hostRate),
      aaFilter(std::max(1, profile.aaFilterPoles), 20.0, hostRate) {
    const int poles = std::max(1, static_cast<int>(std::lround(profile.filterSlopeDbPerOctave / 6.0)));
    const int stageCount = std::max(1, profile.filterStageCount);
    filterStages.reserve(static_cast<size_t>(stageCount));
    for (int i = 0; i < stageCount; ++i) {
        // Placeholder cutoff/resonance -- retuneFilter() runs before
        // this chain's first processBlock() call ever executes (see
        // RealtimeChannel::process()), so these values are never
        // actually heard.
        filterStages.push_back(makeFilterStage(profile.filterTopology, 20000.0, 0, hostRate, poles, profile.filterResonanceCompensation01));
    }
    configureRate(0.0f, hostRate);
}

void RealtimeChannel::MachineChain::configureRate(float requestedSampleRateHz, double hostRate) {
    effectiveRateHz = resolveSampleRateHz(machine, requestedSampleRateHz, hostRate);
    // Holds are identity at effectiveRateHz >= hostRate (see .h) -- the
    // gate is an optimisation, not a bypass. The AA filter is retuned
    // either way: it is always in circuit.
    holdStagesActive = effectiveRateHz < hostRate;
    aaFilter.retune(effectiveRateHz * profile.aaFilterCutoffRatio);
    recordHold.configure(effectiveRateHz, hostRate);
    dacHold.configure(effectiveRateHz, hostRate);
}

void RealtimeChannel::MachineChain::retuneFilter(double cutoffHz, int resonanceCode) {
    // transposeRatio is fixed at 1.0 in the real-time path (no
    // transpose/varispeed -- see RealtimeChannel.h), so
    // profile.filterTracksPitch never scales cutoffHz here the way
    // applyFilter's whole-buffer version does; the caller (process())
    // already passes a cutoffHz with no pitch-tracking multiplier.
    for (auto& stage : filterStages) {
        stage->retune(cutoffHz, resonanceCode);
    }
}

void RealtimeChannel::MachineChain::processBlock(float* buf, size_t count, int bitDepthOverride) {
    // The machine's ADC front end is always in circuit -- the input
    // anti-alias filter runs at every effective rate, including rates
    // at or above the host's (a machine re-clocking a host-rate stream
    // still hears its own input filter; only the decimate/hold stages
    // collapse to identity there). Matches applyRecordPath.
    for (size_t i = 0; i < count; ++i) {
        buf[i] = aaFilter.process(buf[i]);
    }
    if (holdStagesActive) {
        recordHold.process(buf, count);
    }

    ConverterSpec spec = converterSpecForMachine(profile);
    if (bitDepthOverride > 0) {
        // Capped at native: the override is a crusher, not an upgrade
        // -- a request above the machine's own depth resolves to the
        // machine's own depth, never cleaner than it.
        spec.bits = std::min(bitDepthOverride, profile.bitDepth);
    }
    quantizeBuffer(buf, count, spec);

    if (holdStagesActive) {
        dacHold.process(buf, count);
    }

    for (size_t i = 0; i < count; ++i) {
        float v = buf[i];
        for (auto& stage : filterStages) {
            v = stage->process(v);
        }
        buf[i] = v;
    }
}

void RealtimeChannel::MachineChain::reset() {
    aaFilter.reset();
    recordHold.reset();
    dacHold.reset();
    for (auto& stage : filterStages) {
        stage->reset();
    }
}

RealtimeChannel::RealtimeChannel(double hostSampleRateHz, size_t maxBlockFrames)
    : _hostSampleRateHz(hostSampleRateHz) {
    // maxBlockFrames isn't needed internally -- the crossfade scratch
    // buffer is fixed at kControlIntervalFrames regardless of host block
    // size (see process()'s chunking). Kept as a constructor parameter
    // anyway to mirror the host's own prepareToPlay(sampleRate,
    // maxBlockSize) contract, so a future need (e.g. a debug-build size
    // assertion) has somewhere to read it from without an API change.
    (void)maxBlockFrames;
    _crossfadeLength = crossfadeLengthFor(hostSampleRateHz);
    _crossfadeScratch.assign(kControlIntervalFrames, 0.0f);

    // Every machine's chain, built HERE (the host's prepareToPlay
    // thread), never inside process() -- see .h.
    _chains.reserve(static_cast<size_t>(AkzMachine_Count));
    for (int i = 0; i < static_cast<int>(AkzMachine_Count); ++i) {
        _chains.push_back(std::make_unique<MachineChain>(static_cast<AkzMachine>(i), hostSampleRateHz));
    }

    _current.machine = AkzMachine_S950;
    _current.bitDepth = 0;
    _current.sampleRateHz = 0.0f;
    _current.filterCutoff01 = 1.0f;
    _current.filterResonance01 = 0.0f;
    _chain = _chains[static_cast<size_t>(_current.machine)].get();
    _smoothedCutoff01 = _current.filterCutoff01;
    _smoothedResonance01 = _current.filterResonance01;
}

void RealtimeChannel::setParams(const AkzRealtimeChannelParams& params) {
    std::lock_guard<std::mutex> lock(_paramsMutex);
    _pendingParams = params;
    _hasPendingParams = true;
}

void RealtimeChannel::_applyPendingParamsIfAny() {
    if (!_paramsMutex.try_lock()) {
        return; // a UI-thread setParams() is mid-write; pick it up next block rather than wait
    }
    if (_hasPendingParams) {
        const AkzMachine oldMachine = _current.machine;
        _current = _pendingParams;
        _hasPendingParams = false;
        if (_current.machine != oldMachine) {
            _machineSwapPending = true;
        }
    }
    _paramsMutex.unlock();
}

void RealtimeChannel::reset() {
    if (_chain) _chain->reset();
    _crossfadeFrom = nullptr; // drop mid-crossfade state rather than resume a stale fade after a transport jump
    _crossfadeRemaining = 0;
    _smoothedCutoff01 = _current.filterCutoff01;
    _smoothedResonance01 = _current.filterResonance01;
}

void RealtimeChannel::process(float* inout, size_t frames) {
    _applyPendingParamsIfAny();

    if (_machineSwapPending) {
        // Pointer swap into the pre-built array -- no allocation. The
        // incoming chain is reset() so it starts from the same zeroed
        // state a freshly built chain used to have (idle chains may
        // hold stale state from an earlier activation).
        MachineChain* incoming = _chains[static_cast<size_t>(_current.machine)].get();
        incoming->reset();
        if (_chain && _chain != incoming) {
            _crossfadeFrom = _chain;
            _crossfadeRemaining = _crossfadeLength;
        }
        _chain = incoming;
        _machineSwapPending = false;
    }

    const double smoothingCoeff = smoothingCoefficientFor(_hostSampleRateHz);

    size_t offset = 0;
    while (offset < frames) {
        size_t chunk = std::min<size_t>(frames - offset, kControlIntervalFrames);
        if (_crossfadeFrom) {
            chunk = std::min(chunk, _crossfadeRemaining);
        }
        if (chunk == 0) break; // crossfade finished exactly on a previous iteration's boundary

        for (size_t s = 0; s < chunk; ++s) {
            _smoothedCutoff01 = static_cast<float>(_smoothedCutoff01 + smoothingCoeff * (_current.filterCutoff01 - _smoothedCutoff01));
            _smoothedResonance01 = static_cast<float>(_smoothedResonance01 + smoothingCoeff * (_current.filterResonance01 - _smoothedResonance01));
        }

        const double cutoffHz = filterStagesMapCutoff01ToHz(_smoothedCutoff01, _hostSampleRateHz);
        const int resonanceCode = resonanceCodeFromSmoothed(_smoothedResonance01);

        _chain->retuneFilter(cutoffHz, resonanceCode);
        _chain->configureRate(_current.sampleRateHz, _hostSampleRateHz);

        float* segment = inout + offset;

        if (_crossfadeFrom) {
            _crossfadeFrom->retuneFilter(cutoffHz, resonanceCode);
            _crossfadeFrom->configureRate(_current.sampleRateHz, _hostSampleRateHz);

            std::copy(segment, segment + chunk, _crossfadeScratch.begin());
            _chain->processBlock(segment, chunk, _current.bitDepth);
            _crossfadeFrom->processBlock(_crossfadeScratch.data(), chunk, _current.bitDepth);

            const size_t fadeStart = _crossfadeLength - _crossfadeRemaining;
            for (size_t s = 0; s < chunk; ++s) {
                const double t = static_cast<double>(fadeStart + s + 1) / static_cast<double>(_crossfadeLength);
                const double newGain = std::sin(t * M_PI / 2.0);
                const double oldGain = std::cos(t * M_PI / 2.0);
                segment[s] = static_cast<float>(segment[s] * newGain + _crossfadeScratch[s] * oldGain);
            }

            _crossfadeRemaining -= chunk;
            if (_crossfadeRemaining == 0) {
                _crossfadeFrom = nullptr;
            }
        } else {
            _chain->processBlock(segment, chunk, _current.bitDepth);
        }

        offset += chunk;
    }
}

} // namespace akz

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

AkzRealtimeChannel* akz_realtime_channel_create(double hostSampleRateHz, size_t maxBlockFrames) {
    return reinterpret_cast<AkzRealtimeChannel*>(new akz::RealtimeChannel(hostSampleRateHz, maxBlockFrames));
}

void akz_realtime_channel_destroy(AkzRealtimeChannel* channel) {
    delete reinterpret_cast<akz::RealtimeChannel*>(channel);
}

void akz_realtime_channel_set_params(AkzRealtimeChannel* channel, const AkzRealtimeChannelParams* params) {
    reinterpret_cast<akz::RealtimeChannel*>(channel)->setParams(*params);
}

void akz_realtime_channel_reset(AkzRealtimeChannel* channel) {
    reinterpret_cast<akz::RealtimeChannel*>(channel)->reset();
}

void akz_realtime_channel_process(AkzRealtimeChannel* channel, float* inout, size_t frames) {
    reinterpret_cast<akz::RealtimeChannel*>(channel)->process(inout, frames);
}
