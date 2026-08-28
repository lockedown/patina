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

    public init(channelCount: Int, sampleRateHz: Double) {
        let players = (0..<max(1, channelCount)).map { _ in RealtimePlayer(sampleRateHz: sampleRateHz) }
        self.players = players

        let format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: sampleRateHz,
            channels: AVAudioChannelCount(max(1, channelCount)),
            interleaved: false
        )!

        sourceNode = AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList in
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
        for (i, samples) in channels.enumerated() where i < players.count {
            players[i].setSource(samples)
        }
    }

    /// UI-thread call. Same idea: takes effect once the background
    /// recompute finishes, with no audible glitch on the render thread in
    /// the meantime (it just keeps looping whatever was last published).
    public func setParams(_ params: AkzStretchParams) {
        for player in players {
            player.setParams(params)
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
}
