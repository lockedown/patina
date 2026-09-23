// RateModel.cpp
//
// See RateModel.h for the record-path rationale. OnePoleLPF and
// converterSpecForMachine used to be defined here (anonymous namespace);
// they now live in RateStages.h so Sources/Core/RealtimeChannel.h can
// reuse them with persistent state across calls. This file's own
// behaviour is unchanged -- see Tests/CoreTests/RateModelTests.cpp.

#include "RateModel.h"
#include "ConverterModel.h"
#include "MachineProfile.h"
#include "RateStages.h"

#include <algorithm>
#include <vector>

namespace akz {

namespace {

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
    (void)hostSampleRateHz; // no longer a resolution target -- see header comment
    const AkzMachineProfile& profile = machineProfile(machine);
    if (requestedSampleRateHz <= 0.0f) {
        return profile.maxSampleRateHz; // machine's own top-end rate -- never a bypass
    }
    const double requested = static_cast<double>(requestedSampleRateHz);
    return std::max(profile.minSampleRateHz, std::min(requested, profile.maxSampleRateHz));
}

void applyRecordPath(float* buffer, size_t count, AkzMachine machine, double effectiveRateHz, double hostSampleRateHz) {
    if (count == 0) return;

    const AkzMachineProfile& profile = machineProfile(machine);

    const ConverterSpec converterSpec = converterSpecForMachine(profile);

    if (effectiveRateHz <= 0.0 || effectiveRateHz >= hostSampleRateHz) {
        // Already at (or above) host rate -- nothing to decimate, and no
        // anti-alias filtering either: a machine running at its own
        // native/host rate should sound identical to before this stage
        // existed, not pick up an incidental low-pass "for free."
        quantizeBuffer(buffer, count, converterSpec);
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

    // Bit-depth (and, if the machine compands, companding) quantise the
    // now rate-limited signal last -- sample rate and converter
    // character are independent ADC properties; either order is
    // physically equivalent, this one keeps the rate stage self-
    // contained rather than splitting it around the caller's own
    // quantizeBuffer call the way v1 had it.
    quantizeBuffer(buffer, count, converterSpec);
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
