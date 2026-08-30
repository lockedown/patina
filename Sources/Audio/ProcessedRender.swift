// ProcessedRender.swift
//
// The offline decode -> per-channel StretchProcessor -> render loop, and
// the encode -> write chain, extracted out of ContentView's process() and
// saveProcessed() so both they and the drag-out export (which needs to
// auto-render a stale/missing processedChannels before handing a file to
// Finder) share one implementation instead of two copies drifting apart.
//
// Lives in AkaizerAudio, not AkaizerSApp, so it's importable from
// XCTest and from a future export type in either target.

import AkaizerCore
import Foundation

public enum ProcessedRender {
    /// Decodes `sample`'s raw PCM and renders every channel through
    /// StretchProcessor with `params`. Each channel is stretched
    /// independently with identical parameters -- the real hardware has
    /// no notion of stereo linkage inside the stretch algorithm itself.
    public static func render(sample: LoadedSample, params: AkzStretchParams) -> [[Float]] {
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let inputChannels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)

        var outputChannels: [[Float]] = []
        outputChannels.reserveCapacity(inputChannels.count)
        for channel in inputChannels {
            let processor = StretchProcessor(sampleRateHz: sample.sampleRateHz)
            processor.setParams(params)
            processor.setSource(channel)
            outputChannels.append(processor.renderAll())
        }
        return outputChannels
    }

    /// Interleaves and encodes `channels` at `format`, then writes a
    /// canonical WAV file -- the same PCMConversion.interleave ->
    /// PCMConversion.fromFloat -> WavCodec.write chain saveProcessed()
    /// used inline before this extraction.
    public static func writeWav(channels: [[Float]], format: WavFormat, to url: URL) throws {
        let interleaved = PCMConversion.interleave(channels)
        let rawData = PCMConversion.fromFloat(interleaved, format: format)
        let wavFile = WavFile(format: format, rawData: rawData)
        try WavCodec.write(wavFile, to: url)
    }
}
