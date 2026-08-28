// WavCodec.swift
//
// A minimal, dependency-free WAV (RIFF/WAVE) reader/writer.
//
// Why this exists instead of just using AVAudioFile: on this machine's
// SDK (macOS 26.5 SDK, Xcode 26.6), AVAudioFile has two reproducible bugs
// that make it unsafe for the bit-exact round trip this app depends on
// (see plan section 8, "offline render and real-time audition ... must
// be bit-identical" -- the same requirement applies to plain file I/O):
//
//   1. Reading with `commonFormat: .pcmFormatInt16` silently truncates
//      the frame count (a 44100-frame/16-bit file came back as 42969
//      frames, reproducibly, across three different AVAudioFile call
//      shapes and ExtAudioFile directly).
//   2. AVAudioFile(forWriting:) inserts JUNK/FLLR alignment padding
//      chunks before the data chunk by default, and a file AVAudioFile
//      wrote this way came back with `file.length == 0` when reopened --
//      AVAudioFile cannot always reliably read its own output.
//
// Reading Float32 (the default processing format) was reliable in
// testing. Rather than build the app's file I/O around "read float32,
// hope the write path behaves," this codec reads and writes raw PCM
// bytes directly, with no framework in between. It intentionally covers
// only the common case (canonical `fmt `/`data` chunk PCM or IEEE float,
// mono or multi-channel, 16/24/32-bit) -- exactly what a WAV or AIFF
// sample library actually contains. See AiffCodec.swift for the AIFF
// side (same rationale, big-endian chunk layout instead).

import Foundation

public struct WavFormat: Equatable {
    public let sampleRate: Double
    public let channelCount: Int
    public let bitsPerSample: Int
    public let isFloat: Bool
}

public struct WavFile {
    public let format: WavFormat
    /// Raw interleaved PCM bytes exactly as they appear in the file's
    /// data chunk -- little-endian, packed per bitsPerSample. Kept
    /// verbatim (rather than immediately converted to Float32) so a
    /// pure load-then-save round trip can be byte-for-byte, not merely
    /// "close enough after a format conversion."
    public let rawData: Data

    public var frameCount: Int {
        let bytesPerSample = bitsPerSample / 8
        guard bytesPerSample > 0, format.channelCount > 0 else { return 0 }
        return rawData.count / (bytesPerSample * format.channelCount)
    }

    public var bitsPerSample: Int { format.bitsPerSample }
}

public enum WavCodecError: Error, CustomStringConvertible {
    case notARiffFile(URL)
    case notAWaveFile(URL)
    case missingFmtChunk(URL)
    case missingDataChunk(URL)
    case unsupportedFormat(audioFormatTag: UInt16, bitsPerSample: UInt16)
    case truncatedFile(URL)

    public var description: String {
        switch self {
        case .notARiffFile(let url): return "\(url.lastPathComponent) is not a RIFF file"
        case .notAWaveFile(let url): return "\(url.lastPathComponent) is not a WAVE file"
        case .missingFmtChunk(let url): return "\(url.lastPathComponent) has no fmt chunk"
        case .missingDataChunk(let url): return "\(url.lastPathComponent) has no data chunk"
        case .unsupportedFormat(let tag, let bits):
            return "Unsupported WAV format (formatTag=\(tag), bitsPerSample=\(bits)) -- only PCM int and IEEE float are supported"
        case .truncatedFile(let url): return "\(url.lastPathComponent) is truncated or malformed"
        }
    }
}

public enum WavCodec {
    private static let kFormatPCM: UInt16 = 1
    private static let kFormatIEEEFloat: UInt16 = 3
    private static let kFormatExtensible: UInt16 = 0xFFFE

    public static func read(url: URL) throws -> WavFile {
        let data = try Data(contentsOf: url)
        guard data.count >= 12 else { throw WavCodecError.truncatedFile(url) }
        guard data[0..<4].elementsEqual("RIFF".utf8) else { throw WavCodecError.notARiffFile(url) }
        guard data[8..<12].elementsEqual("WAVE".utf8) else { throw WavCodecError.notAWaveFile(url) }

        var format: WavFormat?
        var payload: Data?

        // Walk the chunk list starting right after "WAVE". Unknown chunks
        // (LIST/INFO, JUNK, FLLR, bext, etc.) are skipped by their
        // declared size -- exactly the chunks that tripped up AVAudioFile
        // above, handled here the boring, correct way: read the size,
        // skip that many bytes, move on.
        var offset = 12
        while offset + 8 <= data.count {
            let chunkID = data[offset..<offset + 4]
            let chunkSize = Int(_readUInt32LE(data, at: offset + 4))
            let bodyStart = offset + 8
            guard bodyStart + chunkSize <= data.count else {
                // Some encoders leave a data chunk's declared size larger
                // than what's actually on disk (e.g. streamed writers that
                // never patched the header). Clamp rather than fail.
                if chunkID.elementsEqual("data".utf8) {
                    payload = data[bodyStart..<data.count]
                }
                break
            }
            let body = data[bodyStart..<bodyStart + chunkSize]

            if chunkID.elementsEqual("fmt ".utf8) {
                format = try _parseFmtChunk(body, url: url)
            } else if chunkID.elementsEqual("data".utf8) {
                payload = body
            }
            // Chunks are word-aligned: an odd-sized chunk is followed by a pad byte.
            offset = bodyStart + chunkSize + (chunkSize % 2)
        }

        guard let format else { throw WavCodecError.missingFmtChunk(url) }
        guard let payload else { throw WavCodecError.missingDataChunk(url) }

        return WavFile(format: format, rawData: Data(payload))
    }

    public static func write(_ file: WavFile, to url: URL) throws {
        var out = Data()
        out.append(contentsOf: "RIFF".utf8)
        let dataSize = UInt32(file.rawData.count)
        let riffSize = UInt32(4 + 24 + 8 + Int(dataSize)) // "WAVE" + fmt chunk (24) + data header (8) + payload
        out.append(_uint32LE(riffSize))
        out.append(contentsOf: "WAVE".utf8)

        out.append(contentsOf: "fmt ".utf8)
        out.append(_uint32LE(16)) // canonical fmt chunk size, no extension
        let formatTag: UInt16 = file.format.isFloat ? kFormatIEEEFloat : kFormatPCM
        out.append(_uint16LE(formatTag))
        out.append(_uint16LE(UInt16(file.format.channelCount)))
        out.append(_uint32LE(UInt32(file.format.sampleRate)))
        let bytesPerSample = file.format.bitsPerSample / 8
        let blockAlign = bytesPerSample * file.format.channelCount
        let byteRate = UInt32(file.format.sampleRate) * UInt32(blockAlign)
        out.append(_uint32LE(byteRate))
        out.append(_uint16LE(UInt16(blockAlign)))
        out.append(_uint16LE(UInt16(file.format.bitsPerSample)))

        out.append(contentsOf: "data".utf8)
        out.append(_uint32LE(dataSize))
        out.append(file.rawData)

        try out.write(to: url, options: .atomic)
    }

    // -- chunk parsing helpers --------------------------------------------

    private static func _parseFmtChunk(_ body: Data, url: URL) throws -> WavFormat {
        guard body.count >= 16 else { throw WavCodecError.truncatedFile(url) }
        let base = body.startIndex
        let formatTag = _readUInt16LE(body, at: base)
        let channels = _readUInt16LE(body, at: base + 2)
        let sampleRate = _readUInt32LE(body, at: base + 4)
        let bitsPerSample = _readUInt16LE(body, at: base + 14)

        // WAVE_FORMAT_EXTENSIBLE carries the real tag in a GUID further
        // into the chunk; the first two bytes of that GUID match the
        // classic format tag, which is all we need to distinguish PCM
        // from IEEE float.
        var effectiveTag = formatTag
        if formatTag == kFormatExtensible, body.count >= 26 {
            effectiveTag = _readUInt16LE(body, at: base + 24)
        }

        guard effectiveTag == kFormatPCM || effectiveTag == kFormatIEEEFloat else {
            throw WavCodecError.unsupportedFormat(audioFormatTag: effectiveTag, bitsPerSample: bitsPerSample)
        }

        return WavFormat(
            sampleRate: Double(sampleRate),
            channelCount: Int(channels),
            bitsPerSample: Int(bitsPerSample),
            isFloat: effectiveTag == kFormatIEEEFloat
        )
    }

    private static func _readUInt16LE(_ data: Data, at index: Int) -> UInt16 {
        UInt16(data[index]) | (UInt16(data[index + 1]) << 8)
    }

    private static func _readUInt32LE(_ data: Data, at index: Int) -> UInt32 {
        UInt32(data[index])
            | (UInt32(data[index + 1]) << 8)
            | (UInt32(data[index + 2]) << 16)
            | (UInt32(data[index + 3]) << 24)
    }

    private static func _uint16LE(_ value: UInt16) -> Data {
        Data([UInt8(value & 0xFF), UInt8((value >> 8) & 0xFF)])
    }

    private static func _uint32LE(_ value: UInt32) -> Data {
        Data([
            UInt8(value & 0xFF),
            UInt8((value >> 8) & 0xFF),
            UInt8((value >> 16) & 0xFF),
            UInt8((value >> 24) & 0xFF),
        ])
    }
}
