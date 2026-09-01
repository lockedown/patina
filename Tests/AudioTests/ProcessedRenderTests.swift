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

    /// 2.1 feedback ("click and move start point with mouse"):
    /// render(startFrame:) must equal rendering a manually pre-sliced
    /// sample -- proving the trim happens before the stretch engine ever
    /// sees the dropped frames, not as some after-the-fact crop.
    func testRenderWithStartFrameMatchesManuallySlicedSample() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone2s_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.timeFactorPercent = 100.0
        params.cycleLengthSamples = 100

        let startFrame = 10000
        let trimmed = ProcessedRender.render(sample: sample, params: params, startFrame: startFrame)

        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let fullChannel = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)[0]
        let slicedChannel = Array(fullChannel[startFrame...])
        let slicedRawData = PCMConversion.fromFloat(slicedChannel, format: sample.format)
        let slicedSample = LoadedSample(url: sample.url, format: sample.format, rawData: slicedRawData)
        let expected = ProcessedRender.render(sample: slicedSample, params: params)

        XCTAssertEqual(trimmed.count, expected.count)
        for (a, b) in zip(trimmed, expected) {
            XCTAssertEqual(a, b)
        }
    }

    func testRenderWithNegativeStartFrameClampsToZero() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone2s_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)
        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.cycleLengthSamples = 100

        let withNegative = ProcessedRender.render(sample: sample, params: params, startFrame: -500)
        let withZero = ProcessedRender.render(sample: sample, params: params, startFrame: 0)
        XCTAssertEqual(withNegative, withZero)
    }

    func testRenderWithStartFramePastEndProducesEmptyOutput() throws {
        let fixture = fixturesDirectory.appendingPathComponent("tone2s_44k_16bit_mono.wav")
        let service = AudioFileService()
        let sample = try service.load(url: fixture)
        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.cycleLengthSamples = 100

        let result = ProcessedRender.render(sample: sample, params: params, startFrame: sample.frameCount + 1000)
        XCTAssertEqual(result.count, 1)
        XCTAssertEqual(result[0].count, 0)
    }

    /// 2.1 feedback ("Still splitting channels and phasing when in
    /// realtime edit mode"): INTELLIGENT mode's SOLA search normally
    /// picks its splice offset from each channel's OWN content, so two
    /// different channels of one stereo file can and do choose different
    /// offsets -- stereo decorrelation. render() guides every channel
    /// from a shared mid-signal analysis pass instead (see its private
    /// _spliceGuide helper); this pins the actual observable effect end-
    /// to-end: driving StretchProcessor by hand per channel, but WITH
    /// that same guide applied, must reproduce render()'s output exactly
    /// -- proving render() really did link the channels, not just that
    /// it happened to still work.
    func testRenderGuidesIntelligentModeSpliceOffsetsAcrossStereoChannels() throws {
        let frameCount = 20000
        let sampleRateHz = 44100.0
        let channelL: [Float] = (0..<frameCount).map { i in
            Float(sin(2.0 * Double.pi * 220.0 * Double(i) / sampleRateHz)) * 0.7
        }
        let channelR: [Float] = (0..<frameCount).map { i in
            Float(sin(2.0 * Double.pi * 830.0 * Double(i) / sampleRateHz)) * 0.7
        }

        let format = WavFormat(sampleRate: sampleRateHz, channelCount: 2, bitsPerSample: 16, isFloat: false)
        let interleaved = PCMConversion.interleave([channelL, channelR])
        let rawData = PCMConversion.fromFloat(interleaved, format: format)
        let sample = LoadedSample(
            url: URL(fileURLWithPath: "/dev/null"), format: format, rawData: rawData
        )

        var params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        params.mode = AkzStretchMode_Intelligent
        params.timeFactorPercent = 150.0
        params.quality = 60
        params.width = 50

        let rendered = ProcessedRender.render(sample: sample, params: params)
        XCTAssertEqual(rendered.count, 2)

        // render() works from the DECODED channels (rawData round-tripped
        // through 16-bit quantisation), not the pristine floats above --
        // reproduce that same decode before rebuilding the mid signal, or
        // this comparison would be against the wrong input entirely.
        let decodedInterleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let decodedChannels = PCMConversion.deinterleave(decodedInterleaved, channelCount: 2)
        let decodedL = decodedChannels[0]
        let decodedR = decodedChannels[1]

        // Reproduce _spliceGuide's own steps: the mid signal, and the
        // guide engine's lastSpliceOffsets after a render over it.
        var mid = [Float](repeating: 0, count: frameCount)
        for i in 0..<frameCount { mid[i] = (decodedL[i] + decodedR[i]) * 0.5 }
        let guideEngine = StretchProcessor(sampleRateHz: sampleRateHz)
        guideEngine.setParams(params)
        guideEngine.setSource(mid)
        _ = guideEngine.renderAll()
        let guide = guideEngine.lastSpliceOffsets
        XCTAssertFalse(guide.isEmpty)

        for (channel, samples) in [decodedL, decodedR].enumerated() {
            let manual = StretchProcessor(sampleRateHz: sampleRateHz)
            manual.setSpliceGuide(guide)
            manual.setParams(params)
            manual.setSource(samples)
            XCTAssertEqual(rendered[channel], manual.renderAll(), "channel \(channel) didn't use the shared guide")
        }

        // And the guide really did change something real: an unguided,
        // independent render of R must differ from the guided one --
        // otherwise this whole test would pass trivially regardless of
        // whether linkage does anything.
        let unguidedR = StretchProcessor(sampleRateHz: sampleRateHz)
        unguidedR.setParams(params)
        unguidedR.setSource(decodedR)
        XCTAssertNotEqual(rendered[1], unguidedR.renderAll())
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
