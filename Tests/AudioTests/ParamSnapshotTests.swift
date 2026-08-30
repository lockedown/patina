// ParamSnapshotTests.swift
//
// ParamSnapshot exists specifically because AkzStretchParams (a plain C
// struct) has no Equatable of its own -- these tests pin the two
// assumptions the whole undo/staleness/revert design leans on: that the
// round trip through the C struct is lossless, and that the imported C
// enums really do give Equatable synthesis the fine-grained discrimination
// it needs (a snapshot that "==" when it shouldn't would silently break
// undo coalescing and render-staleness detection alike).

import XCTest

@testable import AkaizerAudio
import AkaizerCore

final class ParamSnapshotTests: XCTestCase {
    private func _sampleSnapshot() -> ParamSnapshot {
        ParamSnapshot(
            machine: AkzMachine_S3200, engine: AkzEngine_Revised, mode: AkzStretchMode_Intelligent,
            stretchPercent: 150, cycleLength: 750, quality: 42, width: 17,
            transposeSemitones: -5.5, filterCutoff: 0.75, filterResonance: 0.25
        )
    }

    func testParamsRoundTripPreservesEveryField() {
        let original = _sampleSnapshot()
        let roundTripped = ParamSnapshot(params: original.params)
        XCTAssertEqual(roundTripped, original)
    }

    /// Guards against the possibility that Equatable synthesis on the
    /// imported C enums doesn't discriminate the way it's assumed to --
    /// changing exactly one field at a time must always produce
    /// inequality, never a false "==" that would silently merge two
    /// different undo steps or hide a stale render.
    func testEqualityDiscriminatesEachFieldIndividually() {
        let base = _sampleSnapshot()

        var machine = base; machine.machine = AkzMachine_S900
        XCTAssertNotEqual(machine, base)

        var engine = base; engine.engine = AkzEngine_Classic
        XCTAssertNotEqual(engine, base)

        var mode = base; mode.mode = AkzStretchMode_Cyclic
        XCTAssertNotEqual(mode, base)

        var stretch = base; stretch.stretchPercent += 1
        XCTAssertNotEqual(stretch, base)

        var cycle = base; cycle.cycleLength += 1
        XCTAssertNotEqual(cycle, base)

        var quality = base; quality.quality += 1
        XCTAssertNotEqual(quality, base)

        var width = base; width.width += 1
        XCTAssertNotEqual(width, base)

        var transpose = base; transpose.transposeSemitones += 0.1
        XCTAssertNotEqual(transpose, base)

        var cutoff = base; cutoff.filterCutoff += 0.01
        XCTAssertNotEqual(cutoff, base)

        var resonance = base; resonance.filterResonance += 0.01
        XCTAssertNotEqual(resonance, base)

        XCTAssertEqual(base, base) // and an identical copy still matches
    }

    func testDefaultsMatchesStretchProcessorDefaultParamsForEveryMachine() {
        for machine in StretchProcessor.allMachines {
            let expected = ParamSnapshot(params: StretchProcessor.defaultParams(machine: machine))
            XCTAssertEqual(ParamSnapshot.defaults(for: machine), expected)
        }
    }

    func testAkaizerPresetSnapshotBridgeMatchesItsOwnParams() {
        let preset = AkaizerPreset(name: "Test", params: _sampleSnapshot().params)
        XCTAssertEqual(preset.snapshot, ParamSnapshot(params: preset.params))
    }
}
