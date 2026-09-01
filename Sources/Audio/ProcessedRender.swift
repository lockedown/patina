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
    /// independently -- the real hardware has no notion of stereo
    /// linkage inside the stretch algorithm itself -- EXCEPT for
    /// INTELLIGENT mode's splice-point search, which is guided by one
    /// shared analysis pass over all channels (see _spliceGuide below):
    /// left to its own devices, INTELLIGENT mode picks its splice offset
    /// by cross-correlating each channel's OWN content, so L and R
    /// routinely choose different offsets at the same nominal position
    /// -- 2.1 feedback's "still splitting channels and phasing." Cyclic
    /// mode's block mapping is purely length-derived and was never
    /// affected; guiding it anyway is a harmless no-op (the guide is
    /// only ever consulted inside INTELLIGENT's own synthesis step).
    /// `startFrame` (2.1 feedback: "click and move start point with
    /// mouse") trims every channel's SOURCE before it's stretched --
    /// what gets rendered and exported is "from the start point to the
    /// end," not the whole file. Clamped rather than trusted: negative
    /// clamps to 0, past-the-end clamps to an empty source (which
    /// StretchEngine already handles, producing empty/near-empty output
    /// rather than crashing). Deliberately NOT a param on AkzStretchParams
    /// -- it's a transport position, not something that belongs in a
    /// saved preset (a preset naming a byte offset into some OTHER file
    /// would be meaningless), so this stays entirely on the Swift side of
    /// the boundary.
    public static func render(sample: LoadedSample, params: AkzStretchParams, startFrame: Int = 0) -> [[Float]] {
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let fullChannels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)
        let clampedStart = min(max(0, startFrame), fullChannels.first?.count ?? 0)
        let inputChannels = fullChannels.map { Array($0[clampedStart...]) }

        let guide = _spliceGuide(forChannels: inputChannels, sampleRateHz: sample.sampleRateHz, params: params)

        var outputChannels: [[Float]] = []
        outputChannels.reserveCapacity(inputChannels.count)
        for channel in inputChannels {
            let processor = StretchProcessor(sampleRateHz: sample.sampleRateHz)
            if let guide {
                processor.setSpliceGuide(guide)
            }
            processor.setParams(params)
            processor.setSource(channel)
            outputChannels.append(processor.renderAll())
        }
        return outputChannels
    }

    /// One shared SOLA splice-offset table, derived from the MID (mean
    /// across channels) signal rather than any one channel's own content
    /// -- so applying it to every channel doesn't privilege L over R or
    /// vice versa. `nil` for mono (nothing to link) or an empty source.
    /// Cost: one extra full render over the mid signal, but it REPLACES
    /// each channel's own correlation search (the dominant per-iteration
    /// cost) rather than adding to it -- for N channels this is one
    /// search instead of N, not N+1 searches.
    private static func _spliceGuide(forChannels channels: [[Float]], sampleRateHz: Double, params: AkzStretchParams) -> [Int64]? {
        guard channels.count > 1, let frameCount = channels.first?.count, frameCount > 0 else { return nil }

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
        _ = guideEngine.renderAll() // force the recompute that populates lastSpliceOffsets; the audio itself is discarded
        let offsets = guideEngine.lastSpliceOffsets
        return offsets.isEmpty ? nil : offsets
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
