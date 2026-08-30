// ProcessedRenderTests.swift
//
// ProcessedRender extracts the decode -> per-channel StretchProcessor ->
// render loop out of ContentView's process(), so both it and the
// drag-out export (ProcessedWavExport.swift) share one implementation.
// These tests pin two things: that render() is equivalent to driving
// StretchProcessor by hand per channel (the same steps process() used to
// do inline), and that writeWav's output survives a real load/reload --
// including from an AIFF-sourced sample, which is the endianness claim
// the drag-out feature's "always export .wav, regardless of source
// container" decision leans on (AiffCodec normalises to little-endian on
// read; PCMConversion.fromFloat always emits little-endian, so nothing
// AIFF-specific should leak through).

import Foundation
import XCTest

@testable import AkaizerAudio
import AkaizerCore

final class ProcessedRenderTests: XCTestCase {
    private var fixturesDirectory: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("Fixtures")
    }

    func testRenderMatchesDrivingStretchProcessorPerChannelByHand() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone2s_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.timeFactorPercent = 200.0
        params.cycleLengthSamples = 500

        let rendered = ProcessedRender.render(sample: sample, params: params)

        // Same steps process() used to do inline, driven directly here.
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let inputChannels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)
        var expected: [[Float]] = []
        for channel in inputChannels {
            let processor = StretchProcessor(sampleRateHz: sample.sampleRateHz)
            processor.setParams(params)
            processor.setSource(channel)
            expected.append(processor.renderAll())
        }

        XCTAssertEqual(rendered.count, expected.count)
        for (a, b) in zip(rendered, expected) {
            XCTAssertEqual(a, b)
        }
    }

    func testWriteWavReloadsWithMatchingFormatAndFrameCount() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone2s_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S950)
        params.timeFactorPercent = 150.0
        let channels = ProcessedRender.render(sample: sample, params: params)

        let tempURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("wav")
        defer { try? FileManager.default.removeItem(at: tempURL) }

        try ProcessedRender.writeWav(channels: channels, format: sample.format, to: tempURL)
        let reloaded = try service.load(url: tempURL)

        XCTAssertEqual(reloaded.format, sample.format)
        XCTAssertEqual(reloaded.frameCount, channels.first?.count ?? 0)
    }

    /// Pins the endianness claim behind drag-out always exporting .wav
    /// regardless of the source container: an AIFF-sourced render must
    /// reload correctly as a WAV, with no signedness/byte-order leakage.
    func testWriteWavFromAiffSourcedSampleReloadsCorrectly() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone_44k_16bit_mono.aiff")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.timeFactorPercent = 100.0
        params.cycleLengthSamples = 100 // evenly divides the fixture, isolates length checks
        let channels = ProcessedRender.render(sample: sample, params: params)

        let tempURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("wav")
        defer { try? FileManager.default.removeItem(at: tempURL) }

        try ProcessedRender.writeWav(channels: channels, format: sample.format, to: tempURL)
        let reloaded = try service.load(url: tempURL)

        XCTAssertEqual(reloaded.frameCount, channels.first?.count ?? 0)
        let reloadedFloats = PCMConversion.toFloat(reloaded.rawData, format: reloaded.format)
        XCTAssertFalse(reloadedFloats.contains { $0.isNaN || $0.isInfinite })
        // A correctly byte-ordered/signed decode should round-trip within
        // 16-bit quantisation error of what was written -- a signedness
        // or endianness bug here would blow far past this bound (a
        // flipped sign is a ~2.0 error, not ~0.0001).
        for (a, b) in zip(reloadedFloats, channels.first ?? []) {
            XCTAssertEqual(a, b, accuracy: 1.0 / 32768.0 * 1.5)
        }
    }
}
