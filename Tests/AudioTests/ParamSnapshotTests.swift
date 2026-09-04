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

    // -- adapted(to:) --------------------------------------------------------
    //
    // 2.4 change request: "maintain knob settings between emulators."
    // adapted(to:) replaces _selectMachine's old reset-to-defaults with a
    // carry-over: clamp a knob into range if the target machine shows it,
    // leave it completely alone if not. These tests pin the two rules
    // themselves, the round-trip losslessness that's the actual point of
    // the feature, and the idempotence _selectMachine's call site leans on
    // (see ContentView.swift's own comment on why it calls adapted(to:)
    // even though _applySnapshot self-adapts too).

    /// A value with every field already inside every machine's own range
    /// (both bandwidth and stretch), so adapted(to:) becomes a pure
    /// machine + descriptor-clamp no-op -- nothing here should ever move.
    private func _universallyInRangeSnapshot(machine: AkzMachine, mode: AkzStretchMode) -> ParamSnapshot {
        ParamSnapshot(
            machine: machine, engine: AkzEngine_Revised, mode: mode,
            stretchPercent: 100, cycleLength: 500, quality: 50, width: 50,
            transposeSemitones: 5, filterCutoff: 0.5, filterResonance: 0.5,
            sampleRateHz: 22050
        )
    }

    /// The headline requirement: a value parked on a machine that hides
    /// its knob must come back byte-identical once the round trip lands
    /// back on a machine that shows it again.
    func testAdaptedRoundTripThroughAMachineWithoutTheKnobRestoresTheValueExactly() {
        var original = _universallyInRangeSnapshot(machine: AkzMachine_S950, mode: AkzStretchMode_Cyclic)
        original.sampleRateHz = 12345
        original.stretchPercent = 500

        let roundTripped = original
            .adapted(to: AkzMachine_SP1200) // no bandwidth, no stretch knob
            .adapted(to: AkzMachine_S950)

        XCTAssertEqual(roundTripped, original)
    }

    func testAdaptedIsAPureMachineSwapWhenEveryValueIsUniversallyInRange() {
        for from in StretchProcessor.allMachines {
            for to in StretchProcessor.allMachines {
                for mode in [AkzStretchMode_Cyclic, AkzStretchMode_Intelligent] {
                    let base = _universallyInRangeSnapshot(machine: from, mode: mode)
                    var expected = base
                    expected.machine = to
                    XCTAssertEqual(base.adapted(to: to), expected, "\(from) -> \(to), mode \(mode)")
                }
            }
        }
    }

    func testAdaptedClampsBandwidthIntoTheTargetMachinesOwnRange() {
        var high = _universallyInRangeSnapshot(machine: AkzMachine_S950, mode: AkzStretchMode_Cyclic)
        high.sampleRateHz = 48000 // S950's own max
        XCTAssertEqual(high.adapted(to: AkzMachine_S900).sampleRateHz, 40000) // S900's max

        var low = _universallyInRangeSnapshot(machine: AkzMachine_S950, mode: AkzStretchMode_Cyclic)
        low.sampleRateHz = 7500 // S950's own min
        XCTAssertEqual(low.adapted(to: AkzMachine_Mirage).sampleRateHz, 10000) // Mirage's min
    }

    func testAdaptedClampsStretchIntoTheTargetMachinesOwnMaximum() {
        var s1000 = _universallyInRangeSnapshot(machine: AkzMachine_S1000, mode: AkzStretchMode_Cyclic)
        s1000.stretchPercent = 1500
        XCTAssertEqual(s1000.adapted(to: AkzMachine_S950).stretchPercent, 999)

        var s950 = _universallyInRangeSnapshot(machine: AkzMachine_S950, mode: AkzStretchMode_Cyclic)
        s950.stretchPercent = 999 // S950's own max
        // Widening the range on the way to S1000 must never move a value.
        XCTAssertEqual(s950.adapted(to: AkzMachine_S1000).stretchPercent, 999)
    }

    /// Stretch is inaudible and invisible on a machine with no stretch
    /// knob -- StretchEngine.cpp gates it on supportsTimeStretch before
    /// ever reading timeFactorPercent -- so a parked value must survive
    /// completely untouched, not clamped into some hidden range.
    func testAdaptedLeavesStretchParkedOnAMachineWithNoStretchKnob() {
        var s1000 = _universallyInRangeSnapshot(machine: AkzMachine_S1000, mode: AkzStretchMode_Cyclic)
        s1000.stretchPercent = 1500
        XCTAssertEqual(s1000.adapted(to: AkzMachine_SP1200).stretchPercent, 1500)
    }

    /// Regression pin for the deleted force-to-CYCLIC: a machine with no
    /// mode switch (S950) must not clobber an INTELLIGENT selection that
    /// belongs to a machine still ahead in the round trip.
    func testAdaptedPreservesModeAndEngineThroughAMachineWithNoModeSwitch() {
        let original = _universallyInRangeSnapshot(machine: AkzMachine_S1000, mode: AkzStretchMode_Intelligent)
        let roundTripped = original
            .adapted(to: AkzMachine_S950) // no CYCLIC/INTELLIGENT switch
            .adapted(to: AkzMachine_S1000)
        XCTAssertEqual(roundTripped.mode, AkzStretchMode_Intelligent)
        XCTAssertEqual(roundTripped.engine, original.engine)
    }

    func testAdaptedPutsEveryVisibleKnobInsideItsOwnRange() {
        for to in StretchProcessor.allMachines {
            for mode in [AkzStretchMode_Cyclic, AkzStretchMode_Intelligent] {
                let descriptors = MachineControls.controls(for: to, mode: mode)
                for extreme: Double in [1e9, -1e9] {
                    var base = _universallyInRangeSnapshot(machine: AkzMachine_S950, mode: mode)
                    base.stretchPercent = extreme
                    base.cycleLength = extreme
                    base.quality = extreme
                    base.width = extreme
                    base.transposeSemitones = extreme
                    base.filterCutoff = extreme
                    base.filterResonance = extreme
                    base.sampleRateHz = extreme

                    let adapted = base.adapted(to: to)
                    for descriptor in descriptors {
                        XCTAssertTrue(
                            descriptor.range.contains(adapted[descriptor.id]),
                            "\(descriptor.id) out of range on \(to)/\(mode) from extreme \(extreme)"
                        )
                    }
                }
            }
        }
    }

    func testAdaptedIsIdempotent() {
        for from in StretchProcessor.allMachines {
            for to in StretchProcessor.allMachines {
                for mode in [AkzStretchMode_Cyclic, AkzStretchMode_Intelligent] {
                    var base = _universallyInRangeSnapshot(machine: from, mode: mode)
                    base.stretchPercent = 1e6
                    base.sampleRateHz = 1e6
                    let once = base.adapted(to: to)
                    XCTAssertEqual(once.adapted(to: to), once, "\(from) -> \(to), mode \(mode)")
                }
            }
        }
    }

    /// Tripwire for the Float sampleRateHz rounding hazard and for a
    /// future defaultCycleLength ever leaving 20...2000: a machine's own
    /// defaults must never be altered by adapting them to itself.
    func testAdaptedLeavesEveryMachinesOwnDefaultsUntouched() {
        for machine in StretchProcessor.allMachines {
            let defaults = ParamSnapshot.defaults(for: machine)
            XCTAssertEqual(defaults.adapted(to: machine), defaults)
        }
    }

    func testSubscriptReadsTheFieldMatchingEachParamID() {
        let s = _sampleSnapshot()
        XCTAssertEqual(s[.transpose], s.transposeSemitones)
        XCTAssertEqual(s[.bandwidth], s.sampleRateHz)
        XCTAssertEqual(s[.cutoff], s.filterCutoff)
        XCTAssertEqual(s[.resonance], s.filterResonance)
        XCTAssertEqual(s[.stretch], s.stretchPercent)
        XCTAssertEqual(s[.cycle], s.cycleLength)
        XCTAssertEqual(s[.quality], s.quality)
        XCTAssertEqual(s[.width], s.width)
    }

    func testSubscriptWriteTouchesOnlyItsOwnField() {
        for id in ParamID.allCases {
            var actual = _sampleSnapshot()
            actual[id] = 999

            // Built independently of the subscript setter under test --
            // if it ever wrote to the wrong field, this direct-mutation
            // copy is what catches it.
            var expected = _sampleSnapshot()
            switch id {
            case .transpose: expected.transposeSemitones = 999
            case .bandwidth: expected.sampleRateHz = 999
            case .cutoff: expected.filterCutoff = 999
            case .resonance: expected.filterResonance = 999
            case .stretch: expected.stretchPercent = 999
            case .cycle: expected.cycleLength = 999
            case .quality: expected.quality = 999
            case .width: expected.width = 999
            }

            XCTAssertEqual(actual, expected, "\(id) write touched an unrelated field")
        }
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
