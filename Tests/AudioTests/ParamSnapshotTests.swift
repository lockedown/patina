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
            transposeSemitones: -5.5, filterCutoff: 0.75, filterResonance: 0.25,
            sampleRateHz: 26040
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

        var rate = base; rate.sampleRateHz += 100
        XCTAssertNotEqual(rate, base)

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

    /// 2.1 feedback: "Cutoff use during preview creates clicks and lag on
    /// longer samples." The realtime worker's cheap filter-only path
    /// (RealtimeStretchPlayer's paramsDifferOnlyInFilter, a memcmp of the
    /// whole AkzStretchParams with the two filter fields zeroed) only
    /// fires when NO other field moved. If .params ever produced a
    /// different bit pattern for an unrelated field just because cutoff
    /// changed -- e.g. Double->Float rounding noise -- every cutoff tick
    /// would silently fall through to a full re-render (which resets the
    /// realtime read position to 0: the "jump to the start" + click the
    /// user is seeing). Pins that a cutoff-only edit really does produce
    /// params differing in filterCutoff01 alone.
    func testChangingOnlyCutoffChangesOnlyFilterCutoffInParams() {
        let base = _sampleSnapshot()
        var moved = base
        moved.filterCutoff = 0.4321

        let baseParams = base.params
        let movedParams = moved.params

        XCTAssertEqual(movedParams.machine, baseParams.machine)
        XCTAssertEqual(movedParams.engine, baseParams.engine)
        XCTAssertEqual(movedParams.mode, baseParams.mode)
        XCTAssertEqual(movedParams.timeFactorPercent, baseParams.timeFactorPercent)
        XCTAssertEqual(movedParams.cycleLengthSamples, baseParams.cycleLengthSamples)
        XCTAssertEqual(movedParams.quality, baseParams.quality)
        XCTAssertEqual(movedParams.width, baseParams.width)
        XCTAssertEqual(movedParams.transposeSemitones, baseParams.transposeSemitones)
        XCTAssertEqual(movedParams.filterResonance01, baseParams.filterResonance01)
        XCTAssertEqual(movedParams.sampleRateHz, baseParams.sampleRateHz)
        XCTAssertNotEqual(movedParams.filterCutoff01, baseParams.filterCutoff01)
    }

    /// Same guard, for resonance -- the cheap path's other legal field.
    func testChangingOnlyResonanceChangesOnlyFilterResonanceInParams() {
        let base = _sampleSnapshot()
        var moved = base
        moved.filterResonance = 0.1234

        let baseParams = base.params
        let movedParams = moved.params

        XCTAssertEqual(movedParams.machine, baseParams.machine)
        XCTAssertEqual(movedParams.engine, baseParams.engine)
        XCTAssertEqual(movedParams.mode, baseParams.mode)
        XCTAssertEqual(movedParams.timeFactorPercent, baseParams.timeFactorPercent)
        XCTAssertEqual(movedParams.cycleLengthSamples, baseParams.cycleLengthSamples)
        XCTAssertEqual(movedParams.quality, baseParams.quality)
        XCTAssertEqual(movedParams.width, baseParams.width)
        XCTAssertEqual(movedParams.transposeSemitones, baseParams.transposeSemitones)
        XCTAssertEqual(movedParams.filterCutoff01, baseParams.filterCutoff01)
        XCTAssertEqual(movedParams.sampleRateHz, baseParams.sampleRateHz)
        XCTAssertNotEqual(movedParams.filterResonance01, baseParams.filterResonance01)
    }

    /// The cheap path also depends on repeated reads of an UNCHANGED
    /// snapshot producing bit-identical params -- ContentView rebuilds
    /// `.params` fresh from @State on every push (_pushLiveParamsIfNeeded),
    /// so if the same Double values ever rounded to different Float bits
    /// across calls, an untouched param would look like it moved.
    func testUnchangedSnapshotProducesBitIdenticalParamsAcrossRepeatedReads() {
        let snapshot = _sampleSnapshot()
        let first = snapshot.params
        let second = snapshot.params
        XCTAssertTrue(memcmp0(first, second))
    }
}

/// Byte-for-byte comparison, mirroring the C++ side's memcmp -- XCTAssertEqual
/// on the imported struct would use its own (absent) Equatable, not this.
private func memcmp0(_ a: AkzStretchParams, _ b: AkzStretchParams) -> Bool {
    withUnsafeBytes(of: a) { aBytes in
        withUnsafeBytes(of: b) { bBytes in
            aBytes.elementsEqual(bBytes)
        }
    }
}
