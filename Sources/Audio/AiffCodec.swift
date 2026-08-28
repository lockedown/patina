// AiffCodec.swift
//
// A minimal, dependency-free AIFF (FORM/AIFF) reader/writer, mirroring
// WavCodec.swift -- see that file's header comment for why this app
// avoids AVAudioFile for bit-exact PCM I/O on this SDK.
//
// Scope: classic AIFF linear PCM only (COMM + SSND chunks), which is what
// real sample libraries and Akai-originated material actually are. AIFC
// (compressed AIFF, e.g. float or IMA4 payloads, a different FORM
// sub-type with its own FVER/compression-type fields) is out of scope
// and reported as a clear "unsupported" error rather than silently
// mishandled -- if that turns out to matter in practice, extend
// _parseCommChunk rather than guessing at the AIFC layout blind.
//
// The one AIFF-specific wrinkle worth understanding before editing this
// file: the sample rate is stored as an 80-bit IEEE extended-precision
// float ("SANE" format), not a plain integer or IEEE-754 double. See
// _readExtended80/_writeExtended80, which use Swift's
// exponent/significand decomposition (exact bit manipulation, not
// log2/pow) so standard audio rates like 44100 round-trip exactly.

import Foundation

public enum AiffCodecError: Error, CustomStringConvertible {
    case notAFormFile(URL)
    case notAiff(URL, formType: String)
    case missingCommChunk(URL)
    case missingSsndChunk(URL)
    case truncatedFile(URL)

    public var description: String {
        switch self {
        case .notAFormFile(let url): return "\(url.lastPathComponent) is not a FORM file"
        case .notAiff(let url, let formType): return "\(url.lastPathComponent) is FORM type '\(formType)', not AIFF (AIFC/compressed AIFF is not supported)"
        case .missingCommChunk(let url): return "\(url.lastPathComponent) has no COMM chunk"
        case .missingSsndChunk(let url): return "\(url.lastPathComponent) has no SSND chunk"
        case .truncatedFile(let url): return "\(url.lastPathComponent) is truncated or malformed"
        }
    }
}

public enum AiffCodec {
    public static func read(url: URL) throws -> WavFile {
        // Reuses WavFile/WavFormat as the in-memory representation -- the
        // container differs (FORM/big-endian vs RIFF/little-endian) but
        // the app only ever needs {format, rawData}, and every other
        // layer (AudioFileService, the DSP core) is deliberately
        // container-agnostic. rawData is always normalised to
        // little-endian here so the rest of the app never needs to care
        // which file format a sample came from.
        let data = try Data(contentsOf: url)
        guard data.count >= 12 else { throw AiffCodecError.truncatedFile(url) }
        guard data[0..<4].elementsEqual("FORM".utf8) else { throw AiffCodecError.notAFormFile(url) }
        let formType = String(decoding: data[8..<12], as: UTF8.self)
        guard formType == "AIFF" else { throw AiffCodecError.notAiff(url, formType: formType) }

        var channelCount: Int?
        var bitsPerSample: Int?
        var sampleRate: Double?
        var soundDataBigEndian: Data?

        var offset = 12
        while offset + 8 <= data.count {
            let chunkID = String(decoding: data[offset..<offset + 4], as: UTF8.self)
            let chunkSize = Int(_readUInt32BE(data, at: offset + 4))
            let bodyStart = offset + 8
            let clampedEnd = min(bodyStart + chunkSize, data.count)
            let body = data[bodyStart..<clampedEnd]

            if chunkID == "COMM" {
                guard body.count >= 18 else { throw AiffCodecError.truncatedFile(url) }
                let base = body.startIndex
                channelCount = Int(_readUInt16BE(body, at: base))
                bitsPerSample = Int(_readUInt16BE(body, at: base + 6))
                sampleRate = _readExtended80(body, at: base + 8)
            } else if chunkID == "SSND" {
                guard body.count >= 8 else { throw AiffCodecError.truncatedFile(url) }
                let dataOffset = Int(_readUInt32BE(body, at: body.startIndex))
                let soundStart = body.startIndex + 8 + dataOffset
                soundDataBigEndian = soundStart <= body.endIndex ? Data(body[soundStart...]) : Data()
            }

            // Chunks are word-aligned: an odd-sized chunk is followed by a pad byte.
            offset = bodyStart + chunkSize + (chunkSize % 2)
        }

        guard let channelCount, let bitsPerSample, let sampleRate else {
            throw AiffCodecError.missingCommChunk(url)
        }
        guard let soundDataBigEndian else { throw AiffCodecError.missingSsndChunk(url) }

        let format = WavFormat(sampleRate: sampleRate, channelCount: channelCount, bitsPerSample: bitsPerSample, isFloat: false)
        let littleEndianData = _swapByteOrder(soundDataBigEndian, bitsPerSample: bitsPerSample)
        return WavFile(format: format, rawData: littleEndianData)
    }

    public static func write(_ file: WavFile, to url: URL) throws {
        precondition(!file.format.isFloat, "Classic AIFF is linear PCM only -- IEEE float AIFF (AIFC) is not supported by this codec")

        let bigEndianData = _swapByteOrder(file.rawData, bitsPerSample: file.format.bitsPerSample)

        var comm = Data()
        comm.append(_uint16BE(UInt16(file.format.channelCount)))
        comm.append(_uint32BE(UInt32(file.frameCount)))
        comm.append(_uint16BE(UInt16(file.format.bitsPerSample)))
        comm.append(_writeExtended80(file.format.sampleRate))

        var ssnd = Data()
        ssnd.append(_uint32BE(0)) // offset
        ssnd.append(_uint32BE(0)) // blockSize
        ssnd.append(bigEndianData)

        var out = Data()
        out.append(contentsOf: "FORM".utf8)
        let commChunkTotal = 8 + comm.count + (comm.count % 2)
        let ssndChunkTotal = 8 + ssnd.count + (ssnd.count % 2)
        let formSize = UInt32(4 + commChunkTotal + ssndChunkTotal) // "AIFF" + both chunks
        out.append(_uint32BE(formSize))
        out.append(contentsOf: "AIFF".utf8)

        out.append(contentsOf: "COMM".utf8)
        out.append(_uint32BE(UInt32(comm.count)))
        out.append(comm)
        if comm.count % 2 != 0 { out.append(0) }

        out.append(contentsOf: "SSND".utf8)
        out.append(_uint32BE(UInt32(ssnd.count)))
        out.append(ssnd)
        if ssnd.count % 2 != 0 { out.append(0) }

        try out.write(to: url, options: .atomic)
    }

    // -- byte order -------------------------------------------------------

    /// AIFF sample data is big-endian on disk; WavFile.rawData is always
    /// little-endian internally (matching WavCodec) so the rest of the
    /// app is format-agnostic. Same swap works both directions since it's
    /// a per-sample byte reversal, not a value transform.
    private static func _swapByteOrder(_ data: Data, bitsPerSample: Int) -> Data {
        let bytesPerSample = bitsPerSample / 8
        guard bytesPerSample > 1 else { return data } // 8-bit has no byte order to swap
        var result = Data(capacity: data.count)
        var i = data.startIndex
        while i + bytesPerSample <= data.endIndex {
            for b in stride(from: bytesPerSample - 1, through: 0, by: -1) {
                result.append(data[i + b])
            }
            i += bytesPerSample
        }
        return result
    }

    // -- 80-bit IEEE extended sample rate ---------------------------------

    private static func _readExtended80(_ data: Data, at index: Int) -> Double {
        let byte0 = data[index]
        let byte1 = data[index + 1]
        let sign: FloatingPointSign = (byte0 & 0x80 != 0) ? .minus : .plus
        let biasedExponent = Int((UInt16(byte0 & 0x7F) << 8) | UInt16(byte1))

        var mantissa: UInt64 = 0
        for b in 0..<8 {
            mantissa = (mantissa << 8) | UInt64(data[index + 2 + b])
        }

        if biasedExponent == 0 && mantissa == 0 {
            return 0.0
        }

        let exponent = biasedExponent - 16383
        let significand = Double(mantissa) / Double(UInt64(1) << 63) // bit 63 is the explicit integer bit; UInt64 avoids Int overflow at bit 63
        return Double(sign: sign, exponent: exponent, significand: significand)
    }

    private static func _writeExtended80(_ value: Double) -> Data {
        var bytes = [UInt8](repeating: 0, count: 10)
        guard value != 0, value.isFinite else { return Data(bytes) }

        let sign: UInt8 = value.sign == .minus ? 0x80 : 0x00
        let exponent = value.exponent
        let significand = value.significand // in [1, 2)
        let mantissa = UInt64((significand * Double(UInt64(1) << 63)).rounded())
        let biasedExponent = UInt16(exponent + 16383)

        bytes[0] = sign | UInt8(biasedExponent >> 8)
        bytes[1] = UInt8(biasedExponent & 0xFF)
        for b in 0..<8 {
            bytes[2 + b] = UInt8((mantissa >> (56 - 8 * b)) & 0xFF)
        }
        return Data(bytes)
    }

    // -- big-endian integer helpers ---------------------------------------

    private static func _readUInt16BE(_ data: Data, at index: Int) -> UInt16 {
        (UInt16(data[index]) << 8) | UInt16(data[index + 1])
    }

    private static func _readUInt32BE(_ data: Data, at index: Int) -> UInt32 {
        (UInt32(data[index]) << 24) | (UInt32(data[index + 1]) << 16) | (UInt32(data[index + 2]) << 8) | UInt32(data[index + 3])
    }

    private static func _uint16BE(_ value: UInt16) -> Data {
        Data([UInt8(value >> 8), UInt8(value & 0xFF)])
    }

    private static func _uint32BE(_ value: UInt32) -> Data {
        Data([UInt8((value >> 24) & 0xFF), UInt8((value >> 16) & 0xFF), UInt8((value >> 8) & 0xFF), UInt8(value & 0xFF)])
    }
}
