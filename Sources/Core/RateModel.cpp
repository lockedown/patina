// RateModel.cpp
//
// See RateModel.h for the record-path rationale.

#include "RateModel.h"
#include "ConverterModel.h"
#include "MachineProfile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace akz {

namespace {

// Deliberately a separate, file-local copy of FilterModel.cpp's
// OnePoleLowpassCascade rather than a shared header: that class lives in
// FilterModel.cpp's anonymous namespace (not exported), and the two
// serve genuinely different roles that happen to share an
// implementation shape -- one is the machine's OUTPUT reconstruction
// filter (post-DAC, in the main signal chain), this one is the ADC-side
// anti-alias filter that runs only inside applyRecordPath, before
// decimation. Extracting a shared class for ~15 lines used by exactly
// two call sites would cost a new header for a saving this small.
class OnePoleLPF {
public:
    OnePoleLPF(int poles, double cutoffHz, double sampleRateHz)
        : _poles(std::max(1, poles)) {
        const double clampedCutoff = std::min(cutoffHz, sampleRateHz * 0.49);
        _a = 1.0 - std::exp(-2.0 * M_PI * clampedCutoff / sampleRateHz);
        _state.assign(static_cast<size_t>(_poles), 0.0);
    }

    float process(float x) {
        double v = static_cast<double>(x);
        for (int i = 0; i < _poles; ++i) {
            _state[static_cast<size_t>(i)] += _a * (v - _state[static_cast<size_t>(i)]);
            v = _state[static_cast<size_t>(i)];
        }
        return static_cast<float>(v);
    }

private:
    int _poles;
    double _a;
    std::vector<double> _state;
};

// Shared by applyRecordPath's decimation step and applyDacPath: true
// decimation to targetRateHz followed by zero-order-hold reconstruction
// back to hostSampleRateHz, computed as one combined pass rather than
// through an intermediate shorter buffer -- see RateModel.h. Reads from
// a filtered copy while writing the held result back into `buffer` in
// place, since reading and writing the same array at different rates in
// a single forward pass would otherwise let already-overwritten
// "future" samples leak into the read side. Caller guarantees
// targetRateHz < hostSampleRateHz (the no-decimation-needed case is each
// caller's own early return, since what happens instead -- quantise
// only, vs. nothing at all -- differs between them).
void holdAtRate(float* buffer, size_t count, double targetRateHz, double hostSampleRateHz) {
    const std::vector<float> source(buffer, buffer + count);
    const double samplesPerTargetSample = hostSampleRateHz / targetRateHz;
    double nextBoundary = 0.0;
    float held = source[0];
    for (size_t i = 0; i < count; ++i) {
        if (static_cast<double>(i) >= nextBoundary) {
            held = source[i];
            nextBoundary += samplesPerTargetSample;
        }
        buffer[i] = held;
    }
}

} // namespace

double resolveSampleRateHz(AkzMachine machine, float requestedSampleRateHz, double hostSampleRateHz) {
    if (requestedSampleRateHz <= 0.0f) {
        return hostSampleRateHz; // "no rate stage" -- see header comment
    }
    const AkzMachineProfile& profile = machineProfile(machine);
    const double requested = static_cast<double>(requestedSampleRateHz);
    return std::max(profile.minSampleRateHz, std::min(requested, profile.maxSampleRateHz));
}

void applyRecordPath(float* buffer, size_t count, AkzMachine machine, double effectiveRateHz, double hostSampleRateHz) {
    if (count == 0) return;

    const AkzMachineProfile& profile = machineProfile(machine);

    if (effectiveRateHz <= 0.0 || effectiveRateHz >= hostSampleRateHz) {
        // Already at (or above) host rate -- nothing to decimate, and no
        // anti-alias filtering either: a machine running at its own
        // native/host rate should sound identical to before this stage
        // existed, not pick up an incidental low-pass "for free."
        quantizeBuffer(buffer, count, profile.bitDepth);
        return;
    }

    // Anti-alias filter, tracking the TARGET rate rather than a fixed
    // cutoff -- see RateModel.h and AkaizerCore.h's aaFilterCutoffRatio
    // doc comment. Runs at hostSampleRateHz since that's still the rate
    // `buffer` is sampled at going into this stage.
    const double aaCutoffHz = effectiveRateHz * profile.aaFilterCutoffRatio;
    {
        OnePoleLPF aa(profile.aaFilterPoles, aaCutoffHz, hostSampleRateHz);
        for (size_t i = 0; i < count; ++i) {
            buffer[i] = aa.process(buffer[i]);
        }
    }

    // True decimation to effectiveRateHz followed by zero-order-hold
    // reconstruction back to hostSampleRateHz -- see holdAtRate's own
    // comment for why this is length-neutral by construction.
    holdAtRate(buffer, count, effectiveRateHz, hostSampleRateHz);

    // Bit-depth quantise the now rate-limited signal last -- sample rate
    // and bit depth are independent ADC properties; either order is
    // physically equivalent, this one keeps the rate stage self-
    // contained rather than splitting it around the caller's own
    // quantizeBuffer call the way v1 had it.
    quantizeBuffer(buffer, count, profile.bitDepth);
}

void applyDacPath(float* buffer, size_t count, AkzMachine machine, double playbackRateHz, double hostSampleRateHz) {
    if (count == 0) return;
    if (playbackRateHz <= 0.0 || playbackRateHz >= hostSampleRateHz) {
        return; // nothing to hold beyond native resolution -- see RateModel.h
    }
    (void)machine; // not yet needed -- every machine uses the same ZOH reconstruction shape; kept for a future per-topology DAC character
    holdAtRate(buffer, count, playbackRateHz, hostSampleRateHz);
}

} // namespace akz
