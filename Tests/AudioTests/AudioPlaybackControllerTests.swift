// AudioPlaybackControllerTests.swift
//
// 2.1 feedback: "Play Original goes wrong after exporting -- played slow
// and other artefacts." Root cause was AudioPlaybackController connecting
// its player -> mainMixerNode exactly once, at the FIRST buffer's format,
// and never reconnecting -- so a later play() at a different sample rate
// or channel count kept scheduling into the stale connection format and
// the engine silently resampled it. needsReconnect() is the pure decision
// behind the fix; it's what's testable without standing up a live
// AVAudioEngine (see the class's own doc comment on that split).

import AVFoundation
import XCTest

@testable import AkaizerAudio

final class AudioPlaybackControllerTests: XCTestCase {
    private func _format(sampleRate: Double, channels: AVAudioChannelCount) -> AVAudioFormat {
        AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate, channels: channels, interleaved: false)!
    }

    func testNothingConnectedAlwaysNeedsReconnect() {
        let incoming = _format(sampleRate: 44100, channels: 1)
        XCTAssertTrue(AudioPlaybackController.needsReconnect(connected: nil, incoming: incoming))
    }

    func testSameFormatDoesNotNeedReconnect() {
        let a = _format(sampleRate: 44100, channels: 2)
        let b = _format(sampleRate: 44100, channels: 2)
        XCTAssertFalse(AudioPlaybackController.needsReconnect(connected: a, incoming: b))
    }

    func testDifferentSampleRateNeedsReconnect() {
        let connected = _format(sampleRate: 44100, channels: 1)
        let incoming = _format(sampleRate: 22050, channels: 1)
        XCTAssertTrue(AudioPlaybackController.needsReconnect(connected: connected, incoming: incoming))
    }

    func testDifferentChannelCountNeedsReconnect() {
        let connected = _format(sampleRate: 44100, channels: 1)
        let incoming = _format(sampleRate: 44100, channels: 2)
        XCTAssertTrue(AudioPlaybackController.needsReconnect(connected: connected, incoming: incoming))
    }
}
