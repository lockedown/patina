// RateStages.h
//
// OnePoleLPF and the decimate+reconstruct primitive used to live as
// private, whole-buffer-only helpers inside RateModel.cpp. Hoisted here
// so Sources/Core/RealtimeChannel.h (the real-time signal path) can hold
// the same primitives alive ACROSS calls -- the whole-buffer versions in
// RateModel.cpp (applyRecordPath/applyDacPath, still using these exact
// classes) reset all state on every call, which is correct for "render
// one whole sample" and wrong for "process 128 host-audio frames at a
// time, forever."
//
// StreamingHold is the persistent-state counterpart of RateModel.cpp's
// file-local holdAtRate(): same recurrence (advance an absolute sample
// index against an absolute next-boundary threshold, exactly as
// holdAtRate does within one buffer), just with both promoted to object
// state instead of loop locals, so the boundary phase survives from one
// process() call into the next. Feeding the same audio through
// StreamingHold in one call or in many smaller calls produces bit-
// identical output -- see Tests/CoreTests/RealtimeChannelTests.cpp's
// block-size-invariance case -- because the recurrence never looks past
// `count`, only at its own carried-over state.

#ifndef AKAIZER_RATE_STAGES_H
#define AKAIZER_RATE_STAGES_H

#include "ConverterModel.h"
#include "MachineProfile.h"
#include "include/AkaizerCore.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace akz {

// See RateModel.cpp's original comment: the ADC-side anti-alias filter
// that runs only inside the record path, ahead of decimation. Same
// retune() discipline as FilterStages.h's classes -- recompute
// coefficients, keep _state.
class OnePoleLPF {
public:
    OnePoleLPF(int poles, double cutoffHz, double sampleRateHz)
        : _poles(std::max(1, poles)), _sampleRateHz(sampleRateHz) {
        _state.assign(static_cast<size_t>(_poles), 0.0);
        retune(cutoffHz);
    }

    float process(float x) {
        double v = static_cast<double>(x);
        for (int i = 0; i < _poles; ++i) {
            _state[static_cast<size_t>(i)] += _a * (v - _state[static_cast<size_t>(i)]);
            v = _state[static_cast<size_t>(i)];
        }
        return static_cast<float>(v);
    }

    void retune(double cutoffHz) {
        const double clampedCutoff = std::min(cutoffHz, _sampleRateHz * 0.49);
        _a = 1.0 - std::exp(-2.0 * M_PI * clampedCutoff / _sampleRateHz);
    }

    void reset() {
        std::fill(_state.begin(), _state.end(), 0.0);
    }

private:
    int _poles;
    double _sampleRateHz;
    double _a = 0.0;
    std::vector<double> _state;
};

// Persistent-state counterpart of RateModel.cpp's holdAtRate(). configure()
// may be called with a new targetRateHz at any time (a live bandwidth
// knob) WITHOUT resetting phase/held -- the boundary countdown just
// starts using the new spacing from wherever it currently is, the same
// way a real hardware sample-rate control doesn't re-home to sample 0
// when you turn it. reset() (only called on machine swap / transport
// restart) re-homes explicitly.
class StreamingHold {
public:
    void configure(double targetRateHz, double hostSampleRateHz) {
        _samplesPerTarget = hostSampleRateHz / std::max(1.0, targetRateHz);
    }

    void reset() {
        _index = 0.0;
        _nextBoundary = 0.0;
        _held = 0.0f;
    }

    // Always exactly `count` frames in, `count` frames out, in place --
    // same contract as RateModel.h's whole-buffer functions.
    void process(float* buffer, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (_index >= _nextBoundary) {
                _held = buffer[i];
                _nextBoundary += _samplesPerTarget;
            }
            buffer[i] = _held;
            _index += 1.0;
        }
        _rebaseIfNeeded();
    }

private:
    // Both _index and _nextBoundary grow without bound across a long-
    // running real-time session (unlike the whole-buffer original, which
    // never outlives one render). Periodically subtract whole target-
    // sample cycles from both -- preserves the exact phase relationship
    // (same fractional position within the current hold interval) while
    // keeping the magnitudes double can represent exactly, so a
    // multi-hour DAW session never drifts from floating-point rounding
    // in the accumulated threshold.
    void _rebaseIfNeeded() {
        constexpr double kRebaseThreshold = 1.0e9;
        if (_index < kRebaseThreshold) return;
        const double cyclesToDrop = std::floor(_index / _samplesPerTarget);
        const double dropAmount = cyclesToDrop * _samplesPerTarget;
        _index -= dropAmount;
        _nextBoundary -= dropAmount;
    }

    double _samplesPerTarget = 1.0;
    double _index = 0.0;
    double _nextBoundary = 0.0;
    float _held = 0.0f;
};

// Builds the converter spec the record path quantises with -- moved
// here (from RateModel.cpp's anonymous namespace) unchanged, so
// RealtimeChannel can build the same spec a live bit-depth override
// applies on top of. See RateModel.cpp/ConverterModel.h.
inline ConverterSpec converterSpecForMachine(const AkzMachineProfile& profile) {
    ConverterSpec spec;
    spec.bits = profile.bitDepth;
    spec.companding = profile.companded ? Companding::MuLaw : Companding::None;
    return spec;
}

} // namespace akz

#endif // AKAIZER_RATE_STAGES_H
