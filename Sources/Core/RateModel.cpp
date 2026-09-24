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
// through an intermediate shorter buffer -- see RateModel.h. Safe to run
// in place: the read (buffer[i] on a boundary crossing) and the write
// (buffer[i] = held) are always the SAME index, so no already-overwritten
// "future" sample can ever leak into the read side. Caller guarantees
// targetRateHz < hostSampleRateHz (the no-decimation-needed case is each
// caller's own early return, since what happens instead -- quantise
// only, vs. nothing at all -- differs between them).
void holdAtRate(float* buffer, size_t count, double targetRateHz, double hostSampleRateHz) {
    const double samplesPerTargetSample = hostSampleRateHz / targetRateHz;
    double nextBoundary = 0.0;
    float held = buffer[0];
    for (size_t i = 0; i < count; ++i) {
        if (static_cast<double>(i) >= nextBoundary) {
            held = buffer[i];
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
    if (!profile.hasVariableSampleRate && profile.minSampleRateHz < profile.maxSampleRateHz) {
        // A dual-FIXED-rate machine (S1000/S2000/S3000/S3200) has
        // exactly two real rates -- min and max are a switch, not the
        // ends of a continuous range. Snap any request to the nearer
        // bound so a mid-range value can't produce a rate the hardware
        // never had (the old plain clamp admitted them, a known
        // simplification flagged in AkaizerCore.h).
        return (requested - profile.minSampleRateHz) < (profile.maxSampleRateHz - requested)
            ? profile.minSampleRateHz
            : profile.maxSampleRateHz;
    }
    return std::max(profile.minSampleRateHz, std::min(requested, profile.maxSampleRateHz));
}

void applyRecordPath(float* buffer, size_t count, AkzMachine machine, double effectiveRateHz, double hostSampleRateHz) {
    if (count == 0) return;

    const AkzMachineProfile& profile = machineProfile(machine);

    const ConverterSpec converterSpec = converterSpecForMachine(profile);

    if (effectiveRateHz <= 0.0) {
        // Degenerate input (unreachable via resolveSampleRateHz, which
        // always returns a real in-range rate) -- quantise only, since
        // no meaningful AA cutoff can be computed from a non-positive
        // rate.
        quantizeBuffer(buffer, count, converterSpec);
        return;
    }

    // Anti-alias filter, tracking the TARGET rate rather than a fixed
    // cutoff -- see RateModel.h and AkaizerCore.h's aaFilterCutoffRatio
    // doc comment. Runs at hostSampleRateHz since that's still the rate
    // `buffer` is sampled at going into this stage. ALWAYS in circuit,
    // including at effectiveRateHz >= hostSampleRateHz: this is the
    // machine's input front end, not a decimation-only side effect --
    // a machine re-clocking a host-rate stream still hears its own
    // input filter (RealTimeChannel's MachineChain does the same).
    const double aaCutoffHz = effectiveRateHz * profile.aaFilterCutoffRatio;
    {
        OnePoleLPF aa(profile.aaFilterPoles, aaCutoffHz, hostSampleRateHz);
        aa.processBlock(buffer, count);
    }

    // True decimation to effectiveRateHz followed by zero-order-hold
    // reconstruction back to hostSampleRateHz -- see holdAtRate's own
    // comment for why this is length-neutral by construction. Skipped
    // at effectiveRateHz >= hostSampleRateHz, where the hold is
    // mathematical identity (every host sample is captured when
    // samplesPerTarget < 1) -- an optimisation, not a bypass.
    if (effectiveRateHz < hostSampleRateHz) {
        holdAtRate(buffer, count, effectiveRateHz, hostSampleRateHz);
    }

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
