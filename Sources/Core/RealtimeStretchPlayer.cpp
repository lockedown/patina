// RealtimeStretchPlayer.cpp
//
// See RealtimeStretchPlayer.h and AkaizerCore.h's "Real-time audition
// player" section for the design rationale.

#include "RealtimeStretchPlayer.h"

#include <algorithm>
#include <cmath>

namespace akz {

namespace {
// 5ms is short enough not to smear a fast cutoff sweep into audible
// crossfade artefacts of its own, and long enough to hide a hard sample
// discontinuity -- the standard range for a declick fade.
constexpr double kCrossfadeSeconds = 0.005;
}

RealtimeStretchPlayer::RealtimeStretchPlayer(double sampleRateHz)
    : _engine(sampleRateHz),
      _crossfadeLength(static_cast<size_t>(std::max(1.0, sampleRateHz * kCrossfadeSeconds))) {
    _worker = std::thread(&RealtimeStretchPlayer::_workerLoop, this);
}

RealtimeStretchPlayer::~RealtimeStretchPlayer() {
    _stopRequested.store(true);
    _requestCV.notify_all();
    if (_worker.joinable()) {
        _worker.join();
    }
}

void RealtimeStretchPlayer::setSource(const float* frames, size_t frameCount) {
    std::lock_guard<std::mutex> lock(_requestMutex);
    _pendingSource.assign(frames, frames + frameCount);
    _hasPendingSource = true;
    ++_requestGeneration;
    _requestCV.notify_one();
}

void RealtimeStretchPlayer::setParams(const AkzStretchParams& params) {
    std::lock_guard<std::mutex> lock(_requestMutex);
    _pendingParams = params;
    _hasPendingParams = true;
    ++_requestGeneration;
    _requestCV.notify_one();
}

void RealtimeStretchPlayer::setSpliceGuide(const std::vector<long long>& offsets) {
    std::lock_guard<std::mutex> lock(_requestMutex);
    _pendingSpliceGuide = offsets;
    _hasPendingSpliceGuide = true;
    ++_requestGeneration;
    _requestCV.notify_one();
}

void RealtimeStretchPlayer::_workerLoop() {
    uint64_t lastSeenGeneration = 0;
    std::vector<float> localSource;
    AkzStretchParams localParams{};
    std::vector<long long> localSpliceGuide;
    bool haveSource = false;
    bool haveParams = false;
    bool haveSpliceGuide = false;

    while (true) {
        bool sourceChangedThisIteration = false;
        bool spliceGuideChangedThisIteration = false;
        {
            std::unique_lock<std::mutex> lock(_requestMutex);
            _requestCV.wait(lock, [&] {
                return _stopRequested.load() || _requestGeneration != lastSeenGeneration;
            });
            if (_stopRequested.load()) {
                return;
            }
            lastSeenGeneration = _requestGeneration;

            if (_hasPendingSource) {
                localSource = _pendingSource; // copy out while locked; recompute below happens unlocked
                haveSource = true;
                _hasPendingSource = false;
                sourceChangedThisIteration = true;
            }
            if (_hasPendingParams) {
                localParams = _pendingParams;
                haveParams = true;
                _hasPendingParams = false;
            }
            if (_hasPendingSpliceGuide) {
                localSpliceGuide = _pendingSpliceGuide;
                haveSpliceGuide = true;
                _hasPendingSpliceGuide = false;
                spliceGuideChangedThisIteration = true;
            }
        }

        if (!haveSource || !haveParams || localSource.empty()) {
            continue; // nothing to render yet -- e.g. params arrived before a source was ever loaded
        }

        // A real job is confirmed from here through the publish below --
        // set for the whole render, not just the (possibly slow) full
        // path, so isRecomputing() reports true for the cheap filter-only
        // path too, however briefly.
        _recomputing.store(true, std::memory_order_relaxed);

        // Only a filter/resonance-only change (dragging Cutoff/Resonance
        // while live audition is running -- the reported "restarts on
        // every slide movement" case) can take the cheap path: redo just
        // StretchEngine's last pipeline stage against its cached
        // pre-filter buffer instead of the full stretch/resample/
        // transpose. It requires an existing render to redo, and no new
        // source since then (a new source always needs the full path
        // regardless of what the params say).
        // A splice-guide change alone (2.1 stereo linkage) also forces the
        // full path: the guide only affects INTELLIGENT mode's synthesis
        // step, which the cheap path (reapplyFilterOnly) never touches --
        // taking the cheap path here would silently ignore a guide update.
        const bool canTakeCheapPath = !sourceChangedThisIteration && !spliceGuideChangedThisIteration && _haveRendered
            && StretchEngine::paramsDifferOnlyInFilter(_lastRenderedParams, localParams);

        std::shared_ptr<std::vector<float>> rendered;
        if (canTakeCheapPath) {
            _engine.reapplyFilterOnly(localParams);
            const size_t len = _engine.outputLength();
            rendered = std::make_shared<std::vector<float>>(len);
            const size_t written = _engine.process(rendered->data(), len);
            rendered->resize(written);
        } else {
            // This is the same (allocating) work StretchEngine::process()
            // would do on whatever thread calls it -- here that's always
            // this background thread, never the render thread.
            _engine.reset();
            // Reapplied on every full recompute, not just when it just
            // changed -- same reasoning as localParams/localSource above,
            // which are also always resent regardless of *ThisIteration.
            if (haveSpliceGuide) {
                _engine.setSpliceGuide(localSpliceGuide);
            }
            _engine.setParams(localParams);
            _engine.setSource(localSource.data(), localSource.size());
            const size_t len = _engine.outputLength();
            rendered = std::make_shared<std::vector<float>>(len);
            const size_t written = _engine.process(rendered->data(), len);
            rendered->resize(written);
        }

        // Stash the render as PENDING rather than publishing it here,
        // regardless of which path produced it. A stretch/cycle/mode/
        // transpose change can change the buffer length, which means the
        // read position has to reset to a fixed point (0) rather than
        // remap proportionally -- see the now-historical version of this
        // comment in git blame for the permanent-desync bug that
        // reasoning fixed. But publishing (of either kind) here, the
        // instant THIS channel's own worker finishes, has a bug of its
        // own: LiveAuditionController runs one independent
        // RealtimeStretchPlayer PER CHANNEL, each with its own worker
        // thread racing the others. Whichever channel's worker finishes
        // first would swap in its new buffer immediately, while a
        // sibling channel -- still finishing the SAME change -- keeps
        // playing the OLD one for however long that gap lasts. For that
        // whole window the channels are playing genuinely different
        // audio simultaneously: heard as artificial stereo width/phasing,
        // on every knob that reaches this far, stretch-affecting
        // (Transpose, Stretch, Cycle, Quality, Width, Mode) or, [user
        // feedback, 2026-09] equally, filter-only (Cutoff, Resonance) --
        // the cheap path used to publish straight to _published on the
        // theory that a same-length, position-preserving change needed
        // no cross-channel coordination, which is true of ITS OWN
        // published content but not of WHEN it becomes audible relative
        // to a sibling channel's own cheap-path render of the same knob
        // move.
        //
        // So every render, cheap or full, goes through this same pending
        // slot now. commitPending() (render-thread safe) does the actual
        // publish, and LiveAuditionController's render callback only
        // calls it once every sibling channel also has a pending commit
        // ready -- committing all channels together, in the same
        // callback invocation, so the swap lands on the exact same audio
        // frame for every channel instead of whenever each one's own
        // worker happened to finish.
        _pendingIsFilterOnly.store(canTakeCheapPath, std::memory_order_seq_cst);
        std::atomic_store(&_pendingPublish, std::shared_ptr<const std::vector<float>>(rendered));

        _lastRenderedParams = localParams;
        _haveRendered = true;
        _recomputing.store(false, std::memory_order_relaxed);
    }
}

size_t RealtimeStretchPlayer::pull(float* outFrames, size_t maxOutFrames) {
    std::shared_ptr<const std::vector<float>> buf = std::atomic_load(&_published);
    if (!buf || buf->empty()) {
        std::fill(outFrames, outFrames + maxOutFrames, 0.0f);
        return maxOutFrames;
    }

    size_t pos = _readPos.load(std::memory_order_relaxed);
    const size_t bufLen = buf->size();

    size_t crossfadeRemaining = _crossfadeRemaining.load(std::memory_order_relaxed);
    std::shared_ptr<const std::vector<float>> crossfadeFrom =
        crossfadeRemaining > 0 ? std::atomic_load(&_crossfadeFrom) : nullptr;

    for (size_t i = 0; i < maxOutFrames; ++i) {
        const float newSample = (*buf)[pos];
        if (crossfadeFrom && crossfadeRemaining > 0 && pos < crossfadeFrom->size()) {
            // Equal-power fade: gains are sqrt(t)/sqrt(1-t) rather than a
            // straight linear ramp, so the perceived loudness through the
            // blend stays constant instead of dipping at the midpoint.
            const double t = 1.0 - static_cast<double>(crossfadeRemaining) / static_cast<double>(_crossfadeLength);
            const float gainOld = static_cast<float>(std::sqrt(std::max(0.0, 1.0 - t)));
            const float gainNew = static_cast<float>(std::sqrt(std::max(0.0, t)));
            outFrames[i] = gainOld * (*crossfadeFrom)[pos] + gainNew * newSample;
            --crossfadeRemaining;
        } else {
            outFrames[i] = newSample;
        }
        pos = (pos + 1 == bufLen) ? 0 : pos + 1; // loop -- audition plays continuously, not one-shot
    }
    _readPos.store(pos, std::memory_order_relaxed);
    _crossfadeRemaining.store(crossfadeRemaining, std::memory_order_relaxed);
    return maxOutFrames;
}

bool RealtimeStretchPlayer::isReady() const {
    return std::atomic_load(&_published) != nullptr;
}

double RealtimeStretchPlayer::readPosition01() const {
    std::shared_ptr<const std::vector<float>> buf = std::atomic_load(&_published);
    if (!buf || buf->empty()) {
        return 0.0;
    }
    const size_t pos = _readPos.load(std::memory_order_relaxed);
    // pos is a loop cursor into buf and is always < buf->size() by
    // construction (pull()'s wraparound, or 0 immediately after a
    // publish/commit) -- clamped defensively anyway rather than trusting
    // that invariant across a future change.
    return static_cast<double>(std::min(pos, buf->size() - 1)) / static_cast<double>(buf->size());
}

bool RealtimeStretchPlayer::hasPendingCommit() const {
    return std::atomic_load(&_pendingPublish) != nullptr;
}

bool RealtimeStretchPlayer::isRecomputing() const {
    return _recomputing.load(std::memory_order_relaxed);
}

void RealtimeStretchPlayer::commitPending() {
    std::shared_ptr<const std::vector<float>> pending = std::atomic_load(&_pendingPublish);
    if (!pending) return;
    // Loaded after the (non-null) pointer, matching how the worker wrote
    // them (flag before pointer) -- see _pendingIsFilterOnly's comment.
    const bool filterOnly = _pendingIsFilterOnly.load(std::memory_order_seq_cst);

    if (filterOnly) {
        // Same length as the outgoing buffer by construction (filter/
        // resonance only affect sample values, never buffer length), so
        // preserve the read position instead of restarting, and crossfade
        // from the outgoing buffer rather than stepping straight to the
        // new one -- same reasoning as the old immediate-publish cheap
        // path, just executed here instead of the instant the worker
        // finished. Reading _readPos fresh here (not a value the worker
        // captured back when it started rendering) is actually more
        // correct than the old code was: playback keeps advancing
        // against the still-published OLD buffer for however long this
        // commit was waiting on a sibling channel, and that advancement
        // must not be discarded.
        std::shared_ptr<const std::vector<float>> outgoing = std::atomic_load(&_published);
        std::atomic_store(&_published, pending);
        const size_t oldPos = _readPos.load(std::memory_order_relaxed);
        const size_t newLen = pending->size();
        _readPos.store(newLen > 0 ? std::min(oldPos, newLen - 1) : 0, std::memory_order_relaxed);
        if (outgoing && !outgoing->empty()) {
            std::atomic_store(&_crossfadeFrom, outgoing);
            _crossfadeRemaining.store(std::min(_crossfadeLength, newLen), std::memory_order_relaxed);
        }
    } else {
        std::atomic_store(&_published, pending);
        _readPos.store(0, std::memory_order_relaxed);
    }

    // Clear it rather than leave the stale pointer around -- otherwise a
    // second commitPending() call with no new render in between would
    // re-publish (harmlessly, since it's the same buffer + already-settled
    // position) but hasPendingCommit() would keep reporting true forever,
    // which would make the caller believe a fresh render is still
    // waiting when none is.
    std::atomic_store(&_pendingPublish, std::shared_ptr<const std::vector<float>>(nullptr));
}

} // namespace akz

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

struct AkzRealtimePlayer {
    akz::RealtimeStretchPlayer impl;
    explicit AkzRealtimePlayer(double sampleRateHz) : impl(sampleRateHz) {}
};

AkzRealtimePlayer* akz_realtime_player_create(double sampleRateHz) {
    return new AkzRealtimePlayer(sampleRateHz);
}

void akz_realtime_player_destroy(AkzRealtimePlayer* player) {
    delete player;
}

void akz_realtime_player_set_source(AkzRealtimePlayer* player, const float* source_frames, size_t frame_count) {
    if (!player) return;
    player->impl.setSource(source_frames, frame_count);
}

void akz_realtime_player_set_params(AkzRealtimePlayer* player, const AkzStretchParams* params) {
    if (!player || !params) return;
    player->impl.setParams(*params);
}

void akz_realtime_player_set_splice_guide(AkzRealtimePlayer* player, const long long* offsets, size_t offset_count) {
    if (!player) return;
    std::vector<long long> guide(offsets, offsets + offset_count);
    player->impl.setSpliceGuide(guide);
}

size_t akz_realtime_player_pull(AkzRealtimePlayer* player, float* out_frames, size_t max_out_frames) {
    if (!player) return 0;
    return player->impl.pull(out_frames, max_out_frames);
}

int akz_realtime_player_is_ready(const AkzRealtimePlayer* player) {
    if (!player) return 0;
    return player->impl.isReady() ? 1 : 0;
}

double akz_realtime_player_read_position01(const AkzRealtimePlayer* player) {
    if (!player) return 0.0;
    return player->impl.readPosition01();
}

int akz_realtime_player_has_pending_commit(const AkzRealtimePlayer* player) {
    if (!player) return 0;
    return player->impl.hasPendingCommit() ? 1 : 0;
}

int akz_realtime_player_is_recomputing(const AkzRealtimePlayer* player) {
    if (!player) return 0;
    return player->impl.isRecomputing() ? 1 : 0;
}

void akz_realtime_player_commit_pending(AkzRealtimePlayer* player) {
    if (!player) return;
    player->impl.commitPending();
}
