// AudioFileService.swift
//
// WAV/AIFF read/write. This is the "null signal path" milestone from the
// project plan (build order stage 2): load a sample, save it back out
// unchanged, and prove the round trip is bit-exact before any DSP
// touches it.
//
// Deliberately does NOT go through AVAudioFile -- see WavCodec.swift's
// header comment for the two reproducible AVAudioFile bugs on this SDK
// that make it unsafe for this app's bit-exactness requirement. Instead
// this reads/writes raw PCM bytes via WavCodec/AiffCodec and treats the
// loaded sample as "format + exact bytes," so a pure load-then-save
// round trip is a Data equality check, not a lossy format conversion in
// disguise.
//
// See PCMConversion.swift for the rawData <-> [Float] conversion used to
// feed AkaizerCore's process() and AVAudioEngine playback (stage 3/4).

import Foundation

public struct LoadedSample {
    public let url: URL
    public let format: WavFormat
    public let rawData: Data // exact PCM bytes, little-endian, as read

    public init(url: URL, format: WavFormat, rawData: Data) {
        self.url = url
        self.format = format
        self.rawData = rawData
    }

    public var sampleRateHz: Double { format.sampleRate }
    public var channelCount: Int { format.channelCount }
    public var bitsPerSample: Int { format.bitsPerSample }

    public var frameCount: Int {
        let bytesPerSample = format.bitsPerSample / 8
        guard bytesPerSample > 0, format.channelCount > 0 else { return 0 }
        return rawData.count / (bytesPerSample * format.channelCount)
    }

    public var durationSeconds: Double {
        format.sampleRate > 0 ? Double(frameCount) / format.sampleRate : 0
    }
}

public enum AudioFileServiceError: Error, CustomStringConvertible {
    case unsupportedExtension(URL)

    public var description: String {
        switch self {
        case .unsupportedExtension(let url):
            return "\(url.lastPathComponent): only .wav and .aiff/.aif are supported"
        }
    }
}

public final class AudioFileService {
    public init() {}

    public func load(url: URL) throws -> LoadedSample {
        let wavFile: WavFile
        switch url.pathExtension.lowercased() {
        case "wav":
            wavFile = try WavCodec.read(url: url)
        case "aiff", "aif":
            wavFile = try AiffCodec.read(url: url)
        default:
            throw AudioFileServiceError.unsupportedExtension(url)
        }
        return LoadedSample(url: url, format: wavFile.format, rawData: wavFile.rawData)
    }

    /// Writes a sample back out with the exact bytes it was loaded with.
    /// With no DSP in between, this must round-trip bit-exact -- that
    /// identity is the whole point of this milestone (plan section 8).
    public func save(_ sample: LoadedSample, to url: URL) throws {
        let wavFile = WavFile(format: sample.format, rawData: sample.rawData)
        switch url.pathExtension.lowercased() {
        case "wav":
            try WavCodec.write(wavFile, to: url)
        case "aiff", "aif":
            try AiffCodec.write(wavFile, to: url)
        default:
            throw AudioFileServiceError.unsupportedExtension(url)
        }
    }
}
