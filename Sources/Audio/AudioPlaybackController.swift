// AudioPlaybackController.swift
//
// Plays de-interleaved Float32 channel data through AVAudioEngine. This
// is a different part of AVFoundation from the AVAudioFile bugs
// documented in WavCodec.swift -- AVAudioEngine's Float32 buffer
// playback is the standard, well-trodden real-time audio path and wasn't
// affected in testing. Building the AVAudioPCMBuffer directly from
// Swift-owned arrays (rather than via AVAudioFile) also means none of
// this goes anywhere near the buggy read/write paths anyway.

import AVFoundation
import Foundation

public final class AudioPlaybackController {
    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private var isConnected = false

    /// Bumped by every play()/stop() call. The completion callback
    /// scheduleBuffer fires captures the generation it was scheduled
    /// under and checks it still matches before calling onFinished --
    /// otherwise a callback from a buffer that stop() (or a newer play())
    /// already cut off would fire onFinished a second time, or fire it
    /// for a playback the user already stopped.
    private var generation = 0

    /// Called on the main thread when a play()-scheduled buffer finishes
    /// on its own (not via stop()). ContentView uses this to clear its
    /// "is anything playing" state without polling.
    public var onFinished: (() -> Void)?

    public init() {}

    /// Plays `channels` (one Float array per channel, all the same
    /// length) at `sampleRateHz`. Stops and replaces whatever was
    /// already playing.
    public func play(channels: [[Float]], sampleRateHz: Double) throws {
        stop()
        guard let firstChannel = channels.first, !firstChannel.isEmpty else { return }

        guard let format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: sampleRateHz,
            channels: AVAudioChannelCount(channels.count),
            interleaved: false
        ) else {
            throw PlaybackError.formatCreationFailed
        }

        guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(firstChannel.count)) else {
            throw PlaybackError.bufferAllocationFailed
        }
        buffer.frameLength = AVAudioFrameCount(firstChannel.count)

        guard let channelData = buffer.floatChannelData else {
            throw PlaybackError.bufferAllocationFailed
        }
        for (ch, samples) in channels.enumerated() {
            samples.withUnsafeBufferPointer { src in
                channelData[ch].update(from: src.baseAddress!, count: samples.count)
            }
        }

        if !isConnected {
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: format)
            isConnected = true
        }
        if !engine.isRunning {
            try engine.start()
        }

        generation += 1
        let thisGeneration = generation
        player.scheduleBuffer(buffer, at: nil, options: [], completionCallbackType: .dataPlayedBack) { [weak self] _ in
            // AVAudioPlayerNode calls this on its own internal queue, not
            // the main thread -- and stop() firing this same buffer's
            // natural end can race a newer play() already having moved
            // the generation on, so both the thread hop and the
            // generation check are needed.
            DispatchQueue.main.async {
                guard let self, self.generation == thisGeneration else { return }
                self.onFinished?()
            }
        }
        player.play()
    }

    public func stop() {
        generation += 1
        player.stop()
    }

    public enum PlaybackError: Error, CustomStringConvertible {
        case formatCreationFailed
        case bufferAllocationFailed

        public var description: String {
            switch self {
            case .formatCreationFailed: return "Could not create an audio format for playback"
            case .bufferAllocationFailed: return "Could not allocate a playback buffer"
            }
        }
    }
}
