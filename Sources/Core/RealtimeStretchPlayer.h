// RealtimeStretchPlayer.h
//
// See AkaizerCore.h's "Real-time audition player" section for the full
// rationale. Short version: this exists because StretchEngine::process()
// recomputes (allocating) on whatever thread calls it when dirty, which
// is correct for offline rendering but unsafe on a CoreAudio render
// thread. RealtimeStretchPlayer owns a background thread that does that
// same recompute work and publishes results for a render thread to pull
// from without ever allocating or blocking there.

#ifndef AKAIZER_REALTIME_STRETCH_PLAYER_H
#define AKAIZER_REALTIME_STRETCH_PLAYER_H

#include "StretchEngine.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace akz {

class RealtimeStretchPlayer {
public:
    explicit RealtimeStretchPlayer(double sampleRateHz);
    ~RealtimeStretchPlayer();

    RealtimeStretchPlayer(const RealtimeStretchPlayer&) = delete;
    RealtimeStretchPlayer& operator=(const RealtimeStretchPlayer&) = delete;

    // -- main/UI thread only ----------------------------------------------
    void setSource(const float* frames, size_t frameCount);
    void setParams(const AkzStretchParams& params);

    // 2.1 stereo splice linkage -- see StretchEngine::setSpliceGuide's doc
    // comment. Requests a background recompute the same way setSource/
    // setParams do; persists across subsequent full recomputes (the
    // worker reapplies it every time, same as it does localParams/
    // localSource) until a later call replaces it.
    void setSpliceGuide(const std::vector<long long>& offsets);

    // -- render thread safe -------------------------------------------------
    size_t pull(float* outFrames, size_t maxOutFrames);
    bool isReady() const;

    // Render-thread safe, non-blocking. 2.1 feedback ("show playback bar
    // over sample waveform"): normalised [0, 1) read position into the
    // currently published buffer, for a live-audition playhead. Computed
    // here (position and length loaded together, in one call) rather
    // than exposed as two separate getters -- two calls could tear
    // across a worker's commitPending() swap and briefly disagree,
    // landing outside [0, 1). Returns 0.0 before the first publish.
    double readPosition01() const;

    // Render-thread safe, non-blocking: true once a stretch-affecting
    // re-render has finished on the worker thread and is waiting to be
    // swapped in via commitPending(). See commitPending()'s comment, and
    // LiveAuditionController.swift, for why the swap is deferred rather
    // than automatic -- this player has no idea whether sibling channels
    // (each with their own independent worker) have finished the SAME
    // change yet, and swapping in unilaterally is exactly the bug this
    // pair of methods exists to fix.
    bool hasPendingCommit() const;

    // Render-thread safe, non-blocking: true while the worker thread is
    // actively re-rendering (from the moment a change is confirmed dirty
    // to the moment its result publishes), false at rest. Unlike
    // isReady() -- which latches true forever after the first publish
    // and so can never report a re-render -- this one genuinely toggles,
    // for a "recomputing" UI indicator on a slow re-render (see
    // AkaizerCore.h's comment on akz_realtime_player_is_recomputing).
    bool isRecomputing() const;

    // Render-thread safe: swaps the pending stretch-affecting re-render
    // into the published buffer and resets the read position to 0.
    // No-op if nothing is pending. The caller MUST only call this once
    // every sibling channel's hasPendingCommit() is also true, and MUST
    // commit every channel in the same render-callback invocation --
    // see LiveAuditionController.swift's render callback.
    void commitPending();

private:
    void _workerLoop();

    // Only ever touched by the worker thread -- this is the same
    // synchronous engine AkzStretchEngine wraps, just never called from
    // any other thread.
    StretchEngine _engine;

    std::thread _worker;
    std::atomic<bool> _stopRequested{false};

    // Main-thread -> worker handoff. The mutex/condvar here are only ever
    // waited on by the worker and locked briefly by the main thread, so
    // blocking is fine on both sides -- neither is the render thread.
    std::mutex _requestMutex;
    std::condition_variable _requestCV;
    std::vector<float> _pendingSource;
    AkzStretchParams _pendingParams{};
    std::vector<long long> _pendingSpliceGuide;
    bool _hasPendingParams = false;
    bool _hasPendingSource = false;
    bool _hasPendingSpliceGuide = false;
    uint64_t _requestGeneration = 0;

    // Worker-thread-only bookkeeping (never touched from the main thread,
    // so no lock needed): the params the last full render used, so the
    // next setParams() can be classified as filter-only vs. needing a
    // full re-render (StretchEngine::paramsDifferOnlyInFilter). See
    // _workerLoop()'s comment for why this matters for live audition.
    AkzStretchParams _lastRenderedParams{};
    bool _haveRendered = false;

    // Set true by the worker once a change is confirmed dirty, cleared
    // once its result has published -- read from the main thread by
    // isRecomputing(), written only by the worker, so a plain atomic
    // (no shared_ptr machinery needed, unlike _published/_pendingPublish
    // above -- this is just a flag, not a buffer).
    std::atomic<bool> _recomputing{false};

    // Worker -> render-thread publish. A local shared_ptr copy in pull()
    // holds a strong reference for the duration of the call, so the
    // worker replacing _published concurrently can never invalidate data
    // pull() is actively reading -- see the header comment in
    // AkaizerCore.h for why this is std::atomic_load/store on a plain
    // shared_ptr rather than atomic<shared_ptr<T>> (a C++20 feature this
    // C++17 project doesn't use), and for the honest tradeoff versus
    // strict wait-free lock-free code.
    std::shared_ptr<const std::vector<float>> _published;
    std::atomic<size_t> _readPos{0};

    // 2.1 feedback ("Cutoff use during preview creates clicks... on longer
    // samples"): the cheap filter-only path (see _workerLoop) swaps a
    // freshly refiltered buffer into _published while playback is
    // mid-stream, with no crossfade -- an audible step at the swap point,
    // worse the longer the buffer (more likely a play position lands
    // right on a swap during a knob drag). Since reapplyFilterOnly can't
    // change length, the outgoing and incoming buffers are index-aligned,
    // so pull() can blend old->new sample-for-sample at the same _readPos
    // it's already walking, over a short window starting the instant the
    // swap happens -- exactly where the click actually occurs, unlike a
    // worker-side fade which would be aligned to whenever the worker
    // guessed playback had reached, not where it really was.
    std::shared_ptr<const std::vector<float>> _crossfadeFrom;
    std::atomic<size_t> _crossfadeRemaining{0};
    size_t _crossfadeLength = 0;

    // Worker -> render-thread handoff for a stretch-affecting re-render,
    // held here rather than published/committed directly -- see
    // commitPending()'s comment above for why. Filter-only cheap-path
    // renders skip this entirely and publish straight to _published,
    // since they never touch length or read position and so need no
    // cross-channel gating.
    std::shared_ptr<const std::vector<float>> _pendingPublish;
};

} // namespace akz

#endif // AKAIZER_REALTIME_STRETCH_PLAYER_H
