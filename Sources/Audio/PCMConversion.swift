// PCMConversion.swift
//
// Converts between the raw PCM bytes WavCodec/AiffCodec deal in (see
// their header comments for why this app avoids AVAudioFile/
// AVAudioConverter for this) and the Float32 interleaved samples the DSP
// core and AVAudioEngine both want. Hand-rolled, same reasoning as the
// codecs: no framework in between, nothing to silently mis-round.
//
// Known limitation: 8-bit PCM is decoded as unsigned (the WAV
// convention); AIFF's 8-bit is actually signed, so an 8-bit AIFF file
// would decode wrong. Not fixed because it hasn't come up -- real sample
// libraries are practically always 16-bit -- but flagged rather than
// silently assumed correct. Fix by giving WavFormat a signedness flag
// before relying on this for 8-bit AIFF.

import Foundation

public enum PCMConversion {
    /// Splits interleaved samples into one array per channel.
    public static func deinterleave(_ samples: [Float], channelCount: Int) -> [[Float]] {
        guard channelCount > 0 else { return [] }
        guard channelCount > 1 else { return [samples] }
        var channels = [[Float]](repeating: [], count: channelCount)
        for ch in 0..<channelCount {
            channels[ch] = [Float](repeating: 0, count: samples.count / channelCount)
        }
        for i in stride(from: 0, to: samples.count - channelCount + 1, by: channelCount) {
            for ch in 0..<channelCount {
                channels[ch][i / channelCount] = samples[i + ch]
            }
        }
        return channels
    }

    /// Interleaves one array per channel back into a single sample
    /// stream. All channels must be the same length.
    public static func interleave(_ channels: [[Float]]) -> [Float] {
        guard let frameCount = channels.first?.count, channels.count > 1 else {
            return channels.first ?? []
        }
        var out = [Float](repeating: 0, count: frameCount * channels.count)
        for frame in 0..<frameCount {
            for (ch, samples) in channels.enumerated() {
                out[frame * channels.count + ch] = samples[frame]
            }
        }
        return out
    }

    /// Decodes raw little-endian PCM bytes to interleaved Float32 samples
    /// normalised to roughly [-1, 1].
    public static func toFloat(_ rawData: Data, format: WavFormat) -> [Float] {
        let bytesPerSample = format.bitsPerSample / 8
        guard bytesPerSample > 0 else { return [] }
        let sampleCount = rawData.count / bytesPerSample
        var out = [Float](repeating: 0, count: sampleCount)

        rawData.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            if format.isFloat {
                switch format.bitsPerSample {
                case 32:
                    for i in 0..<sampleCount {
                        out[i] = raw.loadUnaligned(fromByteOffset: i * 4, as: Float32.self)
                    }
                case 64:
                    for i in 0..<sampleCount {
                        out[i] = Float(raw.loadUnaligned(fromByteOffset: i * 8, as: Float64.self))
                    }
                default:
                    break
                }
                return
            }

            switch format.bitsPerSample {
            case 8:
                // Unsigned, WAV convention -- see the AIFF caveat above.
                for i in 0..<sampleCount {
                    let byte = raw.load(fromByteOffset: i, as: UInt8.self)
                    out[i] = (Float(byte) - 128.0) / 128.0
                }
            case 16:
                for i in 0..<sampleCount {
                    let v = raw.loadUnaligned(fromByteOffset: i * 2, as: Int16.self)
                    out[i] = Float(v) / 32768.0
                }
            case 24:
                for i in 0..<sampleCount {
                    let b0 = Int32(raw.load(fromByteOffset: i * 3, as: UInt8.self))
                    let b1 = Int32(raw.load(fromByteOffset: i * 3 + 1, as: UInt8.self))
                    let b2 = Int32(raw.load(fromByteOffset: i * 3 + 2, as: UInt8.self))
                    var v = b0 | (b1 << 8) | (b2 << 16)
                    if v & 0x800000 != 0 { v |= ~0xFFFFFF } // sign-extend 24 -> 32
                    out[i] = Float(v) / 8388608.0
                }
            case 32:
                for i in 0..<sampleCount {
                    let v = raw.loadUnaligned(fromByteOffset: i * 4, as: Int32.self)
                    out[i] = Float(v) / 2147483648.0
                }
            default:
                break
            }
        }
        return out
    }

    /// Encodes interleaved Float32 samples back to raw little-endian PCM
    /// bytes at the given format. Clamps to the target range rather than
    /// wrapping on overflow.
    public static func fromFloat(_ samples: [Float], format: WavFormat) -> Data {
        let bytesPerSample = format.bitsPerSample / 8
        guard bytesPerSample > 0 else { return Data() }
        var out = Data(capacity: samples.count * bytesPerSample)

        if format.isFloat {
            switch format.bitsPerSample {
            case 32:
                for s in samples {
                    var v = s
                    withUnsafeBytes(of: &v) { out.append(contentsOf: $0) }
                }
            case 64:
                for s in samples {
                    var v = Float64(s)
                    withUnsafeBytes(of: &v) { out.append(contentsOf: $0) }
                }
            default:
                break
            }
            return out
        }

        switch format.bitsPerSample {
        case 8:
            for s in samples {
                let clamped = max(-1.0, min(1.0, s))
                let byte = UInt8(max(0, min(255, Int(clamped * 128.0 + 128.0))))
                out.append(byte)
            }
        case 16:
            for s in samples {
                let clamped = max(-1.0, min(1.0, s))
                var v = Int16(max(-32768, min(32767, Int(clamped * 32768.0))))
                withUnsafeBytes(of: &v) { out.append(contentsOf: $0) }
            }
        case 24:
            for s in samples {
                let clamped = max(-1.0, min(1.0, s))
                let v = Int32(max(-8388608, min(8388607, Int(clamped * 8388608.0))))
                out.append(UInt8(v & 0xFF))
                out.append(UInt8((v >> 8) & 0xFF))
                out.append(UInt8((v >> 16) & 0xFF))
            }
        case 32:
            for s in samples {
                let clamped = Double(max(-1.0, min(1.0, s)))
                var v = Int32(max(-2147483648.0, min(2147483647.0, clamped * 2147483648.0)))
                withUnsafeBytes(of: &v) { out.append(contentsOf: $0) }
            }
        default:
            break
        }
        return out
    }

    // -- loudness matching (build order stage 9, A/B) ------------------------
    //
    // "Compares processed against dry original at matched loudness" --
    // the plan's own words. Plain RMS across every channel, not a
    // perceptual/LUFS measure: good enough to stop a level difference
    // from confounding an ear's judgement of the stretch effect itself,
    // which is the actual goal here, not mastering-grade loudness
    // matching.

    public static func rms(_ channels: [[Float]]) -> Float {
        var sumSquares: Double = 0
        var count = 0
        for channel in channels {
            for sample in channel {
                sumSquares += Double(sample) * Double(sample)
                count += 1
            }
        }
        return count > 0 ? Float(sqrt(sumSquares / Double(count))) : 0
    }

    /// The gain to apply to `channels` so its RMS matches
    /// `referenceRMS`, clamped to `±maxGain` so near-silent audio isn't
    /// amplified into a startling blast on playback.
    public static func matchedGain(_ channels: [[Float]], toMatchRMS referenceRMS: Float, maxGain: Float = 4.0) -> Float {
        let currentRMS = rms(channels)
        guard currentRMS > 0.0001 else { return 1.0 } // effectively silent -- gain is meaningless, leave it alone
        let gain = referenceRMS / currentRMS
        return max(1.0 / maxGain, min(maxGain, gain))
    }

    public static func applyGain(_ channels: [[Float]], gain: Float) -> [[Float]] {
        guard gain != 1.0 else { return channels }
        return channels.map { channel in channel.map { $0 * gain } }
    }
}
