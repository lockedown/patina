// RealtimeChannel.h
//
// The real-time, block-streaming counterpart of StretchEngine's
// whole-buffer chain (applyRecordPath -> applyDacPath -> applyFilter),
// with time-stretch and transpose removed. Built for VstPlugin, a
// Windows VST3 INSERT EFFECT: it colors a DAW channel's live audio with
// one machine's rate/bit-depth/filter character, continuously, forever
// -- there is no "whole sample" to look at in advance the way the app's
// own offline render or live-audition player both assume (see
// AkaizerCore.h's "Real-time audition player" section for why THAT
// player, in turn, still isn't this: it loops a precomputed buffer,
// which needs the buffer's full length known up front too).
//
// What's excluded and why:
//   - Time-stretch: needs the whole source known in advance (SOLA
//     search / cyclic block mapping over the full buffer) -- fine for
//     "load one sample, then play it," structurally impossible for
//     "process whatever the host hands me next."
//   - Transpose/varispeed: changes the audio's duration relative to its
//     input, which an insert effect can't do without a growing/draining
//     buffer. Fixed at ratio 1.0 here, which also means
//     AkzMachineProfile.filterTracksPitch and dacClockTracksPitch (both
//     only meaningful when transposeRatio != 1) never engage.
//
// What's kept -- rate/bandwidth (RateModel's record path), bit depth
// (ConverterModel, with the user's own bit-depth override on top of the
// machine's native value), and filter (FilterModel's per-machine
// topology) -- is exactly the part of a machine's signal path that's
// sample-recurrent: each output sample depends only on the current
// input sample and state carried from the previous one, never on
// knowing the rest of the buffer. See FilterStages.h/RateStages.h for
// the persistent-state primitives this is built from (the SAME per-
// sample math applyFilter/applyRecordPath/applyDacPath use, just kept
// alive across calls instead of rebuilt-and-discarded per render).
//
// Threading contract, deliberately much simpler than
// RealtimeStretchPlayer's background-worker/publish protocol: nothing
// here ever changes buffer length or needs a cross-channel commit gate
// (no stretch, so no re-render, so no "wait for every channel's worker"
// dance). setParams() just publishes a small POD snapshot behind a
// try_lock a UI thread essentially never contends; process() picks it
// up (or doesn't, this block) and NEVER blocks.

#ifndef AKAIZER_REALTIME_CHANNEL_H
#define AKAIZER_REALTIME_CHANNEL_H

#include "include/AkaizerCore.h"
#include "FilterStages.h"
#include "MachineProfile.h"
#include "RateStages.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace akz {

class RealtimeChannel {
public:
    RealtimeChannel(double hostSampleRateHz, size_t maxBlockFrames);

    // Any thread. Never allocates, never blocks the caller, and never
    // blocks process() -- see header comment.
    void setParams(const AkzRealtimeChannelParams& params);

    // Audio thread only. Re-homes every stage's phase/filter memory --
    // for transport stop/restart, NOT for an ordinary parameter move
    // (see AkzRealtimeChannelParams's own field docs in AkaizerCore.h).
    void reset();

    // Audio thread only. Exactly `frames` samples in and out, in place.
    // Never allocates.
    void process(float* inout, size_t frames);

private:
    // One fully-instantiated machine's live chain: AA filter -> record
    // decimate/hold -> quantise -> DAC hold -> filter. Everything here
    // is per-machine-profile-shaped (topology, stage count, pole count,
    // companding), which is exactly what makes a MACHINE change --
    // unlike a cutoff/resonance/bandwidth/bit-depth move -- need a new
    // one of these rather than an in-place retune.
    struct MachineChain {
        MachineChain(AkzMachine machine, double hostSampleRateHz);

        // Re-resolves the effective rate from the (possibly just
        // changed) requested bandwidth and reconfigures the AA filter/
        // record hold/DAC hold to it -- phase-preserving (see
        // RateStages.h's StreamingHold), so this is safe to call every
        // block regardless of whether the request actually changed.
        void configureRate(float requestedSampleRateHz, double hostSampleRateHz);

        // Recomputes every filter stage's coefficients in place --
        // state-preserving (see FilterStages.h), safe to call every
        // control-rate tick.
        void retuneFilter(double cutoffHz, int resonanceCode);

        // Runs the whole chain over `count` in-place samples.
        // bitDepthOverride <= 0 means "use the machine's own native
        // bit depth."
        void processBlock(float* buf, size_t count, int bitDepthOverride);

        void reset();

        const AkzMachine machine;
        const AkzMachineProfile& profile;
        const double hostSampleRateHz;
        double effectiveRateHz = 0.0;
        bool rateStageActive = false;

        OnePoleLPF aaFilter;
        StreamingHold recordHold;
        StreamingHold dacHold;
        std::vector<std::unique_ptr<IFilterStage>> filterStages;
    };

    void _applyPendingParamsIfAny();

    const double _hostSampleRateHz;

    // Audio-thread-owned; never touched from any other thread.
    AkzRealtimeChannelParams _current{};
    std::unique_ptr<MachineChain> _chain;

    // Non-null only while crossfading a just-started machine change's
    // new chain in over the outgoing one -- see .cpp for why a machine
    // change (unlike every other parameter) needs this: the AA filter's
    // pole count and the filter's topology/stage count are per-machine-
    // profile constants, not something an existing chain's stages can
    // just be retuned to.
    std::unique_ptr<MachineChain> _crossfadeFrom;
    size_t _crossfadeRemaining = 0;
    size_t _crossfadeLength = 0;
    std::vector<float> _crossfadeScratch; // fixed kControlIntervalFrames size, never reallocated in process()
    bool _needsRebuild = true; // forces the very first process() call to construct _chain

    // Control-rate cutoff/resonance smoothing -- see .cpp for the
    // per-sample one-pole smoother and the retune cadence.
    float _smoothedCutoff01 = 1.0f;
    float _smoothedResonance01 = 0.0f;

    // Main/UI-thread -> audio-thread handoff. try_lock on the audio
    // side means this NEVER blocks the render thread; a UI-thread call
    // that loses the race simply waits a negligible amount for the
    // next block, since setParams() itself briefly blocks on the same
    // mutex (acceptable -- it's a UI-rate call, never the render
    // thread). See RealtimeStretchPlayer.h's header comment for the
    // more elaborate shared_ptr-publish alternative this deliberately
    // avoids needing: nothing here changes buffer length, so there's
    // no cross-channel commit gate to build.
    std::mutex _paramsMutex;
    AkzRealtimeChannelParams _pendingParams{};
    bool _hasPendingParams = false;
};

} // namespace akz

#endif // AKAIZER_REALTIME_CHANNEL_H
