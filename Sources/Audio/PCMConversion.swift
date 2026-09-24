// PCMConversion.swift
//
// Converts between the raw PCM bytes WavCodec/AiffCodec deal in (see
// their header comments for why this app avoids AVAudioFile/
// AVAudioConverter for this) and the Float32 interleaved samples the DSP
// core and AVAudioEngine both want.
//
// The hot paths (16/32-bit decode, float decode, stereo deinterleave,
// 16-bit encode, applyGain) use Accelerate/vDSP -- every one is
// bit-identical to the scalar loops they replaced: the divisors are all
// powers of two (exact), vDSP_vfix16 truncates toward zero exactly like
// Int(), and the float paths are straight copies. Scalar loops remain
// for 8/24-bit (no exact-fit vDSP primitive) and rms (which deliberately
// accumulates in Double).

import Accelerate
import Foundation

public enum PCMConversion {
    /// Splits interleaved samples into one array per channel.
    public static func deinterleave(_ samples: [Float], channelCount: Int) -> [[Float]] {
        guard channelCount > 0 else { return [] }
        guard channelCount > 1 else { return [samples] }
        if channelCount == 2 {
            // The overwhelmingly common case -- two strided copies
            // (vsmul by 1.0; vDSP_vmov isn't in the Swift overlay, and
            // x*1.0 is bit-identical anyway).
            let frameCount = samples.count / 2
            var a = [Float](repeating: 0, count: frameCount)
            var b = [Float](repeating: 0, count: frameCount)
            var one = Float(1.0)
            samples.withUnsafeBufferPointer { src in
                guard let base = src.baseAddress else { return }
                vDSP_vsmul(base, 2, &one, &a, 1, vDSP_Length(frameCount))
                vDSP_vsmul(base + 1, 2, &one, &b, 1, vDSP_Length(frameCount))
            }
            return [a, b]
        }
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

        if format.isFloat {
            switch format.bitsPerSample {
            case 32:
                // Native Float32 little-endian -- a straight copy.
                _ = out.withUnsafeMutableBufferPointer { dst in
                    rawData.prefix(sampleCount * 4).copyBytes(to: UnsafeMutableRawBufferPointer(dst))
                }
            case 64:
                var tmp = [Double](repeating: 0, count: sampleCount)
                _ = tmp.withUnsafeMutableBufferPointer { dst in
                    rawData.prefix(sampleCount * 8).copyBytes(to: UnsafeMutableRawBufferPointer(dst))
                }
                vDSP_vdpsp(&tmp, 1, &out, 1, vDSP_Length(sampleCount))
            default:
                break
            }
            return out
        }

        switch format.bitsPerSample {
        case 8:
            rawData.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                if format.is8BitSigned {
                    // AIFF convention -- see WavFormat.is8BitSigned.
                    for i in 0..<sampleCount {
                        out[i] = Float(Int8(bitPattern: raw.load(fromByteOffset: i, as: UInt8.self))) / 128.0
                    }
                } else {
                    // Unsigned, WAV convention.
                    for i in 0..<sampleCount {
                        let byte = raw.load(fromByteOffset: i, as: UInt8.self)
                        out[i] = (Float(byte) - 128.0) / 128.0
                    }
                }
            }
        case 16:
            var tmp = [Int16](repeating: 0, count: sampleCount)
            _ = tmp.withUnsafeMutableBufferPointer { dst in
                rawData.prefix(sampleCount * 2).copyBytes(to: UnsafeMutableRawBufferPointer(dst))
            }
            var scale = Float(1.0 / 32768.0)
            vDSP_vflt16(&tmp, 1, &out, 1, vDSP_Length(sampleCount))
            out.withUnsafeMutableBufferPointer { buf in
                vDSP_vsmul(buf.baseAddress!, 1, &scale, buf.baseAddress!, 1, vDSP_Length(sampleCount))
            }
        case 24:
            rawData.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                for i in 0..<sampleCount {
                    let b0 = Int32(raw.load(fromByteOffset: i * 3, as: UInt8.self))
                    let b1 = Int32(raw.load(fromByteOffset: i * 3 + 1, as: UInt8.self))
                    let b2 = Int32(raw.load(fromByteOffset: i * 3 + 2, as: UInt8.self))
                    var v = b0 | (b1 << 8) | (b2 << 16)
                    if v & 0x800000 != 0 { v |= ~0xFFFFFF } // sign-extend 24 -> 32
                    out[i] = Float(v) / 8388608.0
                }
            }
        case 32:
            var tmp = [Int32](repeating: 0, count: sampleCount)
            _ = tmp.withUnsafeMutableBufferPointer { dst in
                rawData.prefix(sampleCount * 4).copyBytes(to: UnsafeMutableRawBufferPointer(dst))
            }
            var scale = Float(1.0 / 2147483648.0)
            vDSP_vflt32(&tmp, 1, &out, 1, vDSP_Length(sampleCount))
            out.withUnsafeMutableBufferPointer { buf in
                vDSP_vsmul(buf.baseAddress!, 1, &scale, buf.baseAddress!, 1, vDSP_Length(sampleCount))
            }
        default:
            break
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
                samples.withUnsafeBytes { out.append(contentsOf: $0) }
            case 64:
                var tmp = [Double](repeating: 0, count: samples.count)
                vDSP_vspdp(samples, 1, &tmp, 1, vDSP_Length(samples.count))
                tmp.withUnsafeBytes { out.append(contentsOf: $0) }
            default:
                break
            }
            return out
        }

        switch format.bitsPerSample {
        case 8:
            if format.is8BitSigned {
                // AIFF convention -- see WavFormat.is8BitSigned.
                for s in samples {
                    let clamped = max(-1.0, min(1.0, s))
                    let v = Int8(max(-128, min(127, Int(clamped * 128.0))))
                    out.append(UInt8(bitPattern: v))
                }
            } else {
                for s in samples {
                    let clamped = max(-1.0, min(1.0, s))
                    let byte = UInt8(max(0, min(255, Int(clamped * 128.0 + 128.0))))
                    out.append(byte)
                }
            }
        case 16:
            // scale -> clip -> truncate-toward-zero, all vectorised.
            // Equivalent to the scalar clamp-then-truncate: any value
            // that would have clamped to [-1,1] first lands inside the
            // [-32768,32767] clip anyway, and vDSP_vfix16 truncates
            // toward zero exactly like Int().
            var scaled = [Float](repeating: 0, count: samples.count)
            var scale = Float(32768.0)
            var lo = Float(-32768.0)
            var hi = Float(32767.0)
            vDSP_vsmul(samples, 1, &scale, &scaled, 1, vDSP_Length(samples.count))
            scaled.withUnsafeMutableBufferPointer { buf in
                vDSP_vclip(buf.baseAddress!, 1, &lo, &hi, buf.baseAddress!, 1, vDSP_Length(samples.count))
            }
            var ints = [Int16](repeating: 0, count: samples.count)
            vDSP_vfix16(&scaled, 1, &ints, 1, vDSP_Length(samples.count))
            ints.withUnsafeBytes { out.append(contentsOf: $0) }
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
        var g = gain
        return channels.map { channel in
            var out = [Float](repeating: 0, count: channel.count)
            vDSP_vsmul(channel, 1, &g, &out, 1, vDSP_Length(channel.count))
            return out
        }
    }
}
