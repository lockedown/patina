// AudioFileServiceTests.swift
//
// Build order stage 2 acceptance test: load real WAV/AIFF fixtures, save
// them back out unchanged, and assert the round trip is bit-identical.
// Uses #filePath to find Tests/Fixtures directly rather than SwiftPM
// resource bundling, since these are dev-only fixtures, never shipped --
// see the project's "no CLI" decision, which is about the shipped app's
// surface, not its test tooling.

import Foundation
import XCTest

@testable import AkaizerAudio

final class AudioFileServiceTests: XCTestCase {
    private var fixturesDirectory: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // AudioFileServiceTests.swift
            .deletingLastPathComponent() // AudioTests/
            .appendingPathComponent("Fixtures")
    }

    private func _assertRoundTripIsBitIdentical(fixtureName: String, expectedSampleRate: Double, expectedChannels: Int) throws {
        let fixture = fixturesDirectory.appendingPathComponent(fixtureName)
        guard FileManager.default.fileExists(atPath: fixture.path) else {
            XCTFail("Missing fixture at \(fixture.path) -- generate it first (see README).")
            return
        }

        let service = AudioFileService()
        let original = try service.load(url: fixture)

        XCTAssertEqual(original.sampleRateHz, expectedSampleRate)
        XCTAssertEqual(original.channelCount, expectedChannels)
        XCTAssertGreaterThan(original.frameCount, 0)

        let tempURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(fixture.pathExtension)
        defer { try? FileManager.default.removeItem(at: tempURL) }

        try service.save(original, to: tempURL)
        let reloaded = try service.load(url: tempURL)

        XCTAssertEqual(original.format, reloaded.format)
        XCTAssertEqual(original.frameCount, reloaded.frameCount)
        XCTAssertEqual(original.rawData, reloaded.rawData, "Round-tripped audio must be bit-identical to the original")
    }

    func testWavRoundTripIsBitIdentical() throws {
        try _assertRoundTripIsBitIdentical(fixtureName: "tone_44k_16bit_mono.wav", expectedSampleRate: 44100, expectedChannels: 1)
    }

    func testAiffRoundTripIsBitIdentical() throws {
        try _assertRoundTripIsBitIdentical(fixtureName: "tone_44k_16bit_mono.aiff", expectedSampleRate: 44100, expectedChannels: 1)
    }
}
