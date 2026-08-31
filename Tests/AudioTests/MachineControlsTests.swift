// MachineControlsTests.swift
//
// v2 heritage-roster plan, stage 9. This is exactly the logic worth
// testing separately from ContentView (which has none): getting the
// right knob COMBINATION per machine/mode, by construction from profile
// capability flags rather than a hand-written chain of `if`s.

import XCTest

@testable import AkaizerAudio
import AkaizerCore

final class MachineControlsTests: XCTestCase {
    private func ids(_ machine: AkzMachine, mode: AkzStretchMode = AkzStretchMode_Cyclic) -> [ParamID] {
        MachineControls.controls(for: machine, mode: mode).map(\.id)
    }

    func testS900HasNoStretchOrResonanceButKeepsBandwidth() {
        // S900 has no time-stretch and no resonance, but DOES have the
        // same variable-bandwidth control as the S950 -- the minimal
        // case is "no stretch cluster," not "no controls at all."
        XCTAssertEqual(ids(AkzMachine_S900), [.transpose, .bandwidth, .cutoff])
    }

    func testS950HasStretchAndBandwidthTogether() {
        // The plan's own defining test case: a machine with BOTH a
        // bandwidth control and time-stretch must show both clusters,
        // not one crowding out the other.
        let result = ids(AkzMachine_S950)
        XCTAssertTrue(result.contains(.bandwidth))
        XCTAssertTrue(result.contains(.stretch))
        XCTAssertTrue(result.contains(.cycle))
        XCTAssertFalse(result.contains(.resonance)) // S950 has no resonance control
    }

    func testS1000HasStretchButNoBandwidthOrResonance() {
        // hasVariableSampleRate == 0 for S1000 (dual fixed rate, not a
        // continuous bandwidth control) -- no Bandwidth knob.
        let result = ids(AkzMachine_S1000)
        XCTAssertFalse(result.contains(.bandwidth))
        XCTAssertFalse(result.contains(.resonance))
        XCTAssertTrue(result.contains(.stretch))
    }

    func testS2000HasResonanceAndStretchButNoBandwidth() {
        let result = ids(AkzMachine_S2000)
        XCTAssertTrue(result.contains(.resonance))
        XCTAssertTrue(result.contains(.stretch))
        XCTAssertFalse(result.contains(.bandwidth))
    }

    func testCyclicModeShowsCycleNotQualityWidth() {
        let result = ids(AkzMachine_S1000, mode: AkzStretchMode_Cyclic)
        XCTAssertTrue(result.contains(.cycle))
        XCTAssertFalse(result.contains(.quality))
        XCTAssertFalse(result.contains(.width))
    }

    func testIntelligentModeShowsQualityWidthNotCycle() {
        let result = ids(AkzMachine_S1000, mode: AkzStretchMode_Intelligent)
        XCTAssertFalse(result.contains(.cycle))
        XCTAssertTrue(result.contains(.quality))
        XCTAssertTrue(result.contains(.width))
    }

    func testS950IgnoresIntelligentModeRequestSinceItHasNoModeSwitch() {
        // S950 has hasModeSwitch == 0 -- requesting Intelligent mode
        // must still yield Cycle, not Quality/Width, matching
        // StretchEngine.cpp's own "params.mode is ignored" contract.
        let result = ids(AkzMachine_S950, mode: AkzStretchMode_Intelligent)
        XCTAssertTrue(result.contains(.cycle))
        XCTAssertFalse(result.contains(.quality))
        XCTAssertFalse(result.contains(.width))
    }

    func testTransposeAndCutoffAreUnconditionalAcrossEveryMachine() {
        for machine in StretchProcessor.allMachines {
            let result = ids(machine)
            XCTAssertTrue(result.contains(.transpose), "\(machine) missing transpose")
            XCTAssertTrue(result.contains(.cutoff), "\(machine) missing cutoff")
        }
    }

    func testBandwidthRangeMatchesProfileSampleRateRange() {
        let profile = StretchProcessor.profile(for: AkzMachine_S950)
        let bandwidth = MachineControls.controls(for: AkzMachine_S950, mode: AkzStretchMode_Cyclic)
            .first { $0.id == .bandwidth }
        XCTAssertNotNil(bandwidth)
        XCTAssertEqual(bandwidth?.range.lowerBound, profile.minSampleRateHz)
        XCTAssertEqual(bandwidth?.range.upperBound, profile.maxSampleRateHz)
    }

    /// Guards the removal of the old `max(25.0, ...)` UI-side clamp: if
    /// a future machine profile sets supportsTimeStretch != 0 with a
    /// maxStretchPercent that doesn't leave a valid 25...N range, this
    /// must fail loudly here rather than silently clamp in the view.
    func testEveryStretchCapableMachineHasAValidStretchRange() {
        for machine in StretchProcessor.allMachines {
            let profile = StretchProcessor.profile(for: machine)
            guard profile.supportsTimeStretch != 0 else { continue }
            XCTAssertGreaterThan(profile.maxStretchPercent, 25.0, "\(machine) has an invalid stretch range")
        }
    }

    func testStretchRangeMatchesProfileMaxStretchPercent() {
        let profile = StretchProcessor.profile(for: AkzMachine_S1000)
        let stretch = MachineControls.controls(for: AkzMachine_S1000, mode: AkzStretchMode_Cyclic)
            .first { $0.id == .stretch }
        XCTAssertEqual(stretch?.range.upperBound, profile.maxStretchPercent)
    }

    /// Acceptance test named in the plan itself: the six Akai machines'
    /// knob combinations, before and after the descriptor mechanism
    /// existed, must match -- i.e. this table reproduces exactly what
    /// ContentView's hand-written `if`s already decided.
    func testSixAkaiMachinesMatchTheirDocumentedCapabilityCombination() {
        let expectations: [(AkzMachine, hasStretch: Bool, hasResonance: Bool, hasBandwidth: Bool)] = [
            (AkzMachine_S900, false, false, true),
            (AkzMachine_S950, true, false, true),
            (AkzMachine_S1000, true, false, false),
            (AkzMachine_S2000, true, true, false),
            (AkzMachine_S3000, true, true, false),
            (AkzMachine_S3200, true, true, false),
        ]
        for (machine, hasStretch, hasResonance, hasBandwidth) in expectations {
            let result = ids(machine)
            XCTAssertEqual(result.contains(.stretch), hasStretch, "\(machine) stretch")
            XCTAssertEqual(result.contains(.resonance), hasResonance, "\(machine) resonance")
            XCTAssertEqual(result.contains(.bandwidth), hasBandwidth, "\(machine) bandwidth")
        }
    }
}
