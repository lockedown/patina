// PCMConversionLoudnessTests.swift
//
// Build order stage 9, A/B loudness matching -- PCMConversion.rms/
// matchedGain/applyGain. See ContentView.swift's playProcessed for how
// these are actually used: Play Original is the unscaled reference,
// Play Processed is scaled to match its RMS.

import Foundation
import XCTest

@testable import AkaizerAudio

final class PCMConversionLoudnessTests: XCTestCase {
    func testRMSOfSilenceIsZero() {
        let silence: [[Float]] = [[Float](repeating: 0, count: 1000)]
        XCTAssertEqual(PCMConversion.rms(silence), 0)
    }

    func testRMSOfConstantAmplitudeEqualsThatAmplitude() {
        // RMS of a signal that's the same magnitude everywhere is just
        // that magnitude, regardless of sign -- the simplest possible
        // check that the formula isn't, say, accidentally averaging
        // signed values (which would cancel positive/negative and give
        // a wrong answer of 0 for this specific input).
        let constant: [[Float]] = [[0.5, -0.5, 0.5, -0.5, 0.5, -0.5]]
        XCTAssertEqual(PCMConversion.rms(constant), 0.5, accuracy: 1e-6)
    }

    func testRMSOfSineMatchesAmplitudeOverSqrt2() {
        // Standard result: RMS of a full-cycle sine at amplitude A is
        // A/sqrt(2). Enough cycles that a coarse discretisation doesn't
        // skew the average.
        let amplitude: Float = 0.8
        var samples = [Float]()
        for i in 0..<10000 {
            samples.append(amplitude * sin(2.0 * Float.pi * 37.0 * Float(i) / 10000.0))
        }
        let expected = amplitude / sqrt(2.0)
        XCTAssertEqual(PCMConversion.rms([samples]), expected, accuracy: 0.01)
    }

    func testRMSCombinesAllChannels() {
        // Multi-channel RMS should be computed over every sample in
        // every channel together, not just the first channel -- a loud
        // channel 2 should move the result even if channel 1 is silent.
        let channels: [[Float]] = [
            [Float](repeating: 0.0, count: 100),
            [Float](repeating: 1.0, count: 100),
        ]
        XCTAssertEqual(PCMConversion.rms(channels), sqrt(0.5), accuracy: 1e-6) // sqrt(mean([0...0, 1...1]))
    }

    func testMatchedGainScalesQuieterSignalUp() {
        let quiet: [[Float]] = [[0.1, -0.1, 0.1, -0.1]]
        let referenceRMS: Float = 0.5 // 5x louder than the quiet signal's RMS
        // maxGain given explicitly, well above 5x: this test is about
        // the scaling math, not the clamp -- testMatchedGainClampsToMaxGain
        // below covers that separately, and this test shouldn't
        // incidentally depend on whatever the default happens to be.
        let gain = PCMConversion.matchedGain(quiet, toMatchRMS: referenceRMS, maxGain: 10.0)
        XCTAssertEqual(gain, 5.0, accuracy: 1e-4)
    }

    func testMatchedGainClampsToMaxGain() {
        // A near-silent-but-not-quite signal would otherwise demand a
        // huge gain to reach a normal reference level -- clamped so
        // "Play Processed" can never surprise-blast the listener.
        let veryQuiet: [[Float]] = [[0.001, -0.001, 0.001, -0.001]]
        let referenceRMS: Float = 0.7
        let gain = PCMConversion.matchedGain(veryQuiet, toMatchRMS: referenceRMS, maxGain: 4.0)
        XCTAssertEqual(gain, 4.0, accuracy: 1e-4)
    }

    func testMatchedGainOnSilenceReturnsUnityRatherThanBlowingUp() {
        let silence: [[Float]] = [[0, 0, 0, 0]]
        let gain = PCMConversion.matchedGain(silence, toMatchRMS: 0.5)
        XCTAssertEqual(gain, 1.0) // no meaningful gain to compute -- leave it alone, don't divide by ~0
    }

    func testApplyGainScalesEverySample() {
        let channels: [[Float]] = [[0.1, 0.2, -0.3]]
        let result = PCMConversion.applyGain(channels, gain: 2.0)
        XCTAssertEqual(result, [[0.2, 0.4, -0.6]])
    }

    func testApplyGainOfOneIsIdentity() {
        let channels: [[Float]] = [[0.1, 0.2, -0.3]]
        XCTAssertEqual(PCMConversion.applyGain(channels, gain: 1.0), channels)
    }

    func testMatchedPlaybackActuallyMatchesReferenceRMS() {
        // End-to-end sanity for the exact sequence playProcessed()
        // performs: compute the reference's RMS, compute matchedGain
        // against it, apply that gain -- and confirm the RESULT's RMS
        // genuinely lands close to the reference, not just that the
        // individual steps run without crashing.
        let original: [[Float]] = [[0.6, -0.6, 0.6, -0.6]]
        let processed: [[Float]] = [[0.15, -0.15, 0.15, -0.15]]

        let referenceRMS = PCMConversion.rms(original)
        let gain = PCMConversion.matchedGain(processed, toMatchRMS: referenceRMS)
        let matched = PCMConversion.applyGain(processed, gain: gain)

        XCTAssertEqual(PCMConversion.rms(matched), referenceRMS, accuracy: 1e-4)
    }
}
