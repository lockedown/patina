// StretchIntegrationTests.swift
//
// Exercises the full chain ContentView drives: load a real file, decode
// to Float32, run it through the C++ stretch engine, re-encode, save,
// and reload -- the same path a user takes pressing Process then Save
// Processed. Tests/CoreTests/ArtifactTests.cpp already proves the
// engine's own DSP is correct in isolation; this proves the Swift-side
// plumbing around it (PCMConversion, StretchProcessor, the save path)
// doesn't lose or corrupt anything in between.

import Foundation
import XCTest

@testable import AkaizerAudio
import AkaizerCore

final class StretchIntegrationTests: XCTestCase {
    private var fixturesDirectory: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("Fixtures")
    }

    func testFullChainLengtheningProducesLongerSaneAudio() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)

        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        XCTAssertEqual(interleaved.count, sample.frameCount) // mono, so frames == samples

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.timeFactorPercent = 200.0
        params.cycleLengthSamples = 500

        let processor = StretchProcessor(sampleRateHz: sample.sampleRateHz)
        processor.setParams(params)
        processor.setSource(interleaved)
        let output = processor.renderAll()

        // ~2x length, quantised to whole 500-sample blocks (plan "2.3").
        XCTAssertGreaterThan(output.count, interleaved.count)
        XCTAssertEqual(output.count % 500, 0)

        // Sanity on the audio itself, not just its length.
        XCTAssertFalse(output.contains { $0.isNaN || $0.isInfinite })
        let peak = output.reduce(0) { max($0, abs($1)) }
        XCTAssertGreaterThan(peak, 0.01) // not silence
        XCTAssertLessThanOrEqual(peak, 1.05) // no wild overshoot from the crossfade math

        // Round trip the processed audio through the save path.
        let rawData = PCMConversion.fromFloat(output, format: sample.format)
        let processedSample = LoadedSample(url: fixture, format: sample.format, rawData: rawData)

        let tempURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("wav")
        defer { try? FileManager.default.removeItem(at: tempURL) }

        try service.save(processedSample, to: tempURL)
        let reloaded = try service.load(url: tempURL)
        XCTAssertEqual(reloaded.frameCount, output.count)
        XCTAssertEqual(reloaded.rawData, rawData)
    }

    func testFullChainShorteningProducesShorterSaneAudio() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone_44k_16bit_mono.aiff")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)

        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S950)
        params.timeFactorPercent = 60.0
        params.cycleLengthSamples = 300

        let processor = StretchProcessor(sampleRateHz: sample.sampleRateHz)
        processor.setParams(params)
        processor.setSource(interleaved)
        let output = processor.renderAll()

        XCTAssertLessThan(output.count, interleaved.count)
        XCTAssertFalse(output.contains { $0.isNaN || $0.isInfinite })
    }

    func testTransposeUpShortensAudioDownLengthensIt() throws {
        // Build order stage 5, exercised through the same StretchProcessor
        // the app's Process button uses -- not just the C API directly
        // (that's InterpolatorTests.cpp's job).
        let fixture = fixturesDirectory.appendingPathComponent("tone2s_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.timeFactorPercent = 100.0 // isolate transpose from the stretch step
        // CLASSIC quantises to whole cycle-length blocks even at 100%
        // (plan "2.3") -- 100 evenly divides the fixture's 88200 frames,
        // so the stretch step itself stays exactly length-preserving and
        // this test measures only the transpose step.
        params.cycleLengthSamples = 100

        params.transposeSemitones = 12.0
        let up = StretchProcessor(sampleRateHz: sample.sampleRateHz)
        up.setParams(params)
        up.setSource(interleaved)
        let upOutput = up.renderAll()

        params.transposeSemitones = -12.0
        let down = StretchProcessor(sampleRateHz: sample.sampleRateHz)
        down.setParams(params)
        down.setSource(interleaved)
        let downOutput = down.renderAll()

        XCTAssertEqual(Double(upOutput.count), Double(interleaved.count) / 2.0, accuracy: 2.0)
        XCTAssertEqual(Double(downOutput.count), Double(interleaved.count) * 2.0, accuracy: 2.0)
        XCTAssertFalse(upOutput.contains { $0.isNaN || $0.isInfinite })
        XCTAssertFalse(downOutput.contains { $0.isNaN || $0.isInfinite })
    }

    func testZeroTransposeIsANoOpOnLength() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone2s_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        XCTAssertEqual(params.transposeSemitones, 0) // the default itself
        // Same reasoning as testTransposeUpShortensAudioDownLengthensIt:
        // needs a cycle length that evenly divides the fixture's frame
        // count so CLASSIC's own quantisation doesn't confound "is
        // transpose a no-op" with "is stretch a no-op."
        params.cycleLengthSamples = 100

        let processor = StretchProcessor(sampleRateHz: sample.sampleRateHz)
        processor.setParams(params)
        processor.setSource(interleaved)
        XCTAssertEqual(processor.outputLength, interleaved.count)
    }

    func testEncodeDecodeRoundTripPreservesFloatSamplesWithinQuantisationError() throws {
        // PCMConversion itself, isolated from the stretch engine: encode
        // a known Float32 signal to 16-bit PCM and back, and confirm the
        // error is bounded by 16-bit quantisation (~1/32768), not
        // something larger indicating a real conversion bug.
        let original: [Float] = (0..<1000).map { i in
            Float(sin(Double(i) * 0.1)) * 0.8
        }
        let format = WavFormat(sampleRate: 44100, channelCount: 1, bitsPerSample: 16, isFloat: false)

        let rawData = PCMConversion.fromFloat(original, format: format)
        let decoded = PCMConversion.toFloat(rawData, format: format)

        XCTAssertEqual(decoded.count, original.count)
        for i in 0..<original.count {
            XCTAssertEqual(decoded[i], original[i], accuracy: 1.0 / 32768.0 * 1.5)
        }
    }
}
