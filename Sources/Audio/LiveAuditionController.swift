// LiveAuditionController.swift
//
// Drives real-time audition: one RealtimePlayer per channel (the C++
// engine is mono-only, same "process each channel independently with
// identical params" approach ContentView's offline path already uses),
// pulled from an AVAudioSourceNode's render callback -- the actual
// CoreAudio render thread. This is the one piece of Swift code in the app
// that runs there, which is why RealtimePlayer.pull() exists at all; see
// AkaizerCore.h's "Real-time audition player" section for why the plain
// offline StretchProcessor can't be used here.
//
// setSource()/setParams() below are called from the UI thread (slider
// onChange, etc.) and just forward to each RealtimePlayer's same-named
// methods, which themselves hand off to the background recompute thread
// per channel -- nothing here blocks the UI either.

import AkaizerCore
import AVFoundation

public final class LiveAuditionController {
    private let engine = AVAudioEngine()
    private let sourceNode: AVAudioSourceNode
    private var players: [RealtimePlayer]
    private var isRunning = false
    private let sampleRateHz: Double

    /// Retained so setParams()/setSource() can each recompute the splice
    /// guide from BOTH, even though only one arrives per call -- see
    /// _pushSpliceGuideIfPossible.
    private var lastChannels: [[Float]]?
    private var lastParams: AkzStretchParams?

    /// Bumped by every _pushSpliceGuideIfPossible() call; a background
    /// guide computation checks this before applying its result, so a
    /// slower/older one finishing after a newer one has started is
    /// dropped instead of clobbering it with a stale guide.
    private var _guideRequestGeneration = 0

    public init(channelCount: Int, sampleRateHz: Double) {
        self.sampleRateHz = sampleRateHz
        let players = (0..<max(1, channelCount)).map { _ in RealtimePlayer(sampleRateHz: sampleRateHz) }
        self.players = players

        let format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: sampleRateHz,
            channels: AVAudioChannelCount(max(1, channelCount)),
            interleaved: false
        )!

        sourceNode = AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList in
            // Cross-channel commit gate. Each channel's RealtimePlayer
            // re-renders a stretch-affecting change (Transpose, Stretch,
            // Cycle, Quality, Width, Mode) on its own independent worker
            // thread, so one channel can finish before another. Letting
            // each player swap its own new buffer in the moment ITS OWN
            // worker finishes -- the old behaviour -- meant one channel
            // could already be playing the new render while a sibling
            // was still finishing the SAME change on the old one: two
            // genuinely different signals playing at once, heard as
            // artificial stereo width, on every stretch-affecting knob
            // (see RealtimeStretchPlayer.cpp's _workerLoop() for the full
            // writeup). Only commit once every channel has a pending
            // render ready, and commit all of them right here, in this
            // one callback invocation -- so every channel's read
            // position resets to 0 on the exact same audio frame instead
            // of whenever its own worker happened to finish.
            if !players.isEmpty && players.allSatisfy({ $0.hasPendingCommit }) {
                for player in players { player.commitPending() }
            }

            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            for (channel, buffer) in buffers.enumerated() {
                guard channel < players.count, let data = buffer.mData else { continue }
                let floatPtr = data.assumingMemoryBound(to: Float.self)
                players[channel].pull(frameCount: Int(frameCount), into: floatPtr)
            }
            return noErr
        }

        engine.attach(sourceNode)
        engine.connect(sourceNode, to: engine.mainMixerNode, format: format)
    }

    /// UI-thread call. Replaces the audio being auditioned; safe to call
    /// while running (the render thread keeps pulling silence/the old
    /// render until the background recompute publishes the new one).
    public func setSource(channels: [[Float]]) {
        lastChannels = channels
        for (i, samples) in channels.enumerated() where i < players.count {
            players[i].setSource(samples)
        }
        _pushSpliceGuideIfPossible()
    }

    /// UI-thread call. Same idea: takes effect once the background
    /// recompute finishes, with no audible glitch on the render thread in
    /// the meantime (it just keeps looping whatever was last published).
    public func setParams(_ params: AkzStretchParams) {
        lastParams = params
        for player in players {
            player.setParams(params)
        }
        _pushSpliceGuideIfPossible()
    }

    /// 2.1 feedback ("Still splitting channels and phasing when in
    /// realtime edit mode"): each channel's RealtimePlayer runs its own
    /// independent StretchEngine, and INTELLIGENT mode's SOLA splice
    /// search picks its offset by cross-correlating THAT channel's own
    /// content -- so L and R routinely choose different offsets at the
    /// same nominal position. This derives one shared guide from the mid
    /// (mean across channels) signal and pushes it to every channel's
    /// player, so they splice identically instead. See
    /// ProcessedRender.swift's _spliceGuide for the offline path's
    /// identical reasoning.
    ///
    /// Needs both the current channels AND params (ratio/quality/width
    /// feed the guide's own analysis plan, see _planIntelligent), which
    /// setSource/setParams receive separately and not necessarily in the
    /// same call -- lastChannels/lastParams retain whichever arrived
    /// most recently so either setter can trigger a recompute from both.
    ///
    /// The guide pass itself runs a FULL StretchProcessor render (record
    /// path, DAC, filter -- all wasted except lastSpliceOffsets) purely
    /// to get at the SOLA search's own result, so this is real work --
    /// running it synchronously here, on the UI thread that setSource/
    /// setParams are documented to never block, would reintroduce
    /// exactly the kind of knob-drag jank this whole background-worker
    /// architecture exists to avoid. So it runs on a background queue,
    /// same generation-guard pattern AudioPlaybackController.swift uses
    /// for its completion callback: a later call bumps
    /// _guideRequestGeneration, and a slower, older computation finishing
    /// after it is dropped rather than clobbering a newer one with a
    /// stale guide.
    private func _pushSpliceGuideIfPossible() {
        guard let channels = lastChannels, let params = lastParams, channels.count > 1,
              let frameCount = channels.first?.count, frameCount > 0 else { return }

        _guideRequestGeneration += 1
        let thisGeneration = _guideRequestGeneration
        let sampleRateHz = self.sampleRateHz

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var mid = [Float](repeating: 0, count: frameCount)
            let scale = 1.0 / Float(channels.count)
            for channel in channels {
                for i in 0..<frameCount {
                    mid[i] += channel[i] * scale
                }
            }

            let guideEngine = StretchProcessor(sampleRateHz: sampleRateHz)
            guideEngine.setParams(params)
            guideEngine.setSource(mid)
            _ = guideEngine.renderAll() // forces the recompute that populates lastSpliceOffsets; the audio itself is discarded
            let offsets = guideEngine.lastSpliceOffsets
            guard !offsets.isEmpty else { return }

            DispatchQueue.main.async {
                guard let self, self._guideRequestGeneration == thisGeneration else { return }
                for player in self.players {
                    player.setSpliceGuide(offsets)
                }
            }
        }
    }

    public func start() throws {
        guard !isRunning else { return }
        try engine.start()
        isRunning = true
    }

    public func stop() {
        guard isRunning else { return }
        engine.stop()
        isRunning = false
    }

    public var running: Bool { isRunning }

    /// True while any channel's player is actively re-rendering. One
    /// channel busy is enough to call the whole thing "recomputing" --
    /// the point is telling the user something is happening, not
    /// precisely which channel.
    public var isRecomputing: Bool {
        players.contains { $0.isRecomputing }
    }

    /// 2.1 feedback ("show playback bar over sample waveform"): channel
    /// 0's normalised read position, same "any one channel is
    /// representative" reasoning as isRecomputing above -- and channels
    /// are commit-gated to reset together (see the render callback), so
    /// they stay in lockstep by construction, not by coincidence. nil
    /// when there's no player to ask (channelCount was 0, clamped to 1
    /// by max(1, channelCount) above, so this is defensive rather than
    /// reachable in practice).
    public var playheadFraction: Double? {
        players.first?.readPosition01
    }
}
