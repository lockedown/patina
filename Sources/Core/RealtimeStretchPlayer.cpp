// RealtimeStretchPlayer.cpp
//
// See RealtimeStretchPlayer.h and AkaizerCore.h's "Real-time audition
// player" section for the design rationale.

#include "RealtimeStretchPlayer.h"

#include <algorithm>

namespace akz {

RealtimeStretchPlayer::RealtimeStretchPlayer(double sampleRateHz)
    : _engine(sampleRateHz) {
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

void RealtimeStretchPlayer::_workerLoop() {
    uint64_t lastSeenGeneration = 0;
    std::vector<float> localSource;
    AkzStretchParams localParams{};
    bool haveSource = false;
    bool haveParams = false;

    while (true) {
        bool sourceChangedThisIteration = false;
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
        }

        if (!haveSource || !haveParams || localSource.empty()) {
            continue; // nothing to render yet -- e.g. params arrived before a source was ever loaded
        }

        // Only a filter/resonance-only change (dragging Cutoff/Resonance
        // while live audition is running -- the reported "restarts on
        // every slide movement" case) can take the cheap path: redo just
        // StretchEngine's last pipeline stage against its cached
        // pre-filter buffer instead of the full stretch/resample/
        // transpose. It requires an existing render to redo, and no new
        // source since then (a new source always needs the full path
        // regardless of what the params say).
        const bool canTakeCheapPath = !sourceChangedThisIteration && _haveRendered
            && StretchEngine::paramsDifferOnlyInFilter(_lastRenderedParams, localParams);

        const std::shared_ptr<const std::vector<float>> previousBuf = std::atomic_load(&_published);
        const size_t oldLen = previousBuf ? previousBuf->size() : 0;
        const size_t oldPos = _readPos.load(std::memory_order_relaxed);

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
            _engine.setParams(localParams);
            _engine.setSource(localSource.data(), localSource.size());
            const size_t len = _engine.outputLength();
            rendered = std::make_shared<std::vector<float>>(len);
            const size_t written = _engine.process(rendered->data(), len);
            rendered->resize(written);
        }

        std::atomic_store(&_published, std::shared_ptr<const std::vector<float>>(rendered));

        const size_t newLen = rendered->size();
        if (canTakeCheapPath) {
            // Same length by construction (filter/resonance only affect
            // sample values, never buffer length) -- leave the read
            // position exactly where it was instead of restarting.
            _readPos.store(newLen > 0 ? std::min(oldPos, newLen - 1) : 0, std::memory_order_relaxed);
        } else {
            // A stretch/cycle/mode/transpose change can change the
            // buffer length. Rather than always snapping back to 0 (the
            // old, unconditional behaviour the "restarts on every slide
            // movement" report was about), remap the position
            // proportionally so playback resumes near the same musical
            // position instead of visibly restarting.
            size_t newPos = 0;
            if (oldLen > 0 && newLen > 0) {
                newPos = static_cast<size_t>((static_cast<double>(oldPos) / static_cast<double>(oldLen)) * static_cast<double>(newLen));
                newPos = std::min(newPos, newLen - 1);
            }
            _readPos.store(newPos, std::memory_order_relaxed);
        }

        _lastRenderedParams = localParams;
        _haveRendered = true;
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
    for (size_t i = 0; i < maxOutFrames; ++i) {
        outFrames[i] = (*buf)[pos];
        pos = (pos + 1 == bufLen) ? 0 : pos + 1; // loop -- audition plays continuously, not one-shot
    }
    _readPos.store(pos, std::memory_order_relaxed);
    return maxOutFrames;
}

bool RealtimeStretchPlayer::isReady() const {
    return std::atomic_load(&_published) != nullptr;
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

size_t akz_realtime_player_pull(AkzRealtimePlayer* player, float* out_frames, size_t max_out_frames) {
    if (!player) return 0;
    return player->impl.pull(out_frames, max_out_frames);
}

int akz_realtime_player_is_ready(const AkzRealtimePlayer* player) {
    if (!player) return 0;
    return player->impl.isReady() ? 1 : 0;
}
