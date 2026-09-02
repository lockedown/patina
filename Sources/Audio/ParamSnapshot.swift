// ParamSnapshot.swift
//
// AkzStretchParams (AkaizerCore.h) is a plain C struct -- no Equatable,
// no Codable, nothing the Clang importer adds for free beyond field
// access. Comparing "have the params changed" (undo coalescing, render-
// staleness, revert) all need equality, so this Swift-side mirror exists
// to carry it. One type serves all three jobs rather than three
// bespoke ones, since they're really the same question asked from
// different call sites: "does this snapshot match that one?"
//
// The three C enums (AkzMachine, AkzEngine, AkzStretchMode) are plain
// (non-NS_ENUM) C enums, which the Clang importer gives a synthesized
// Equatable/Hashable/RawRepresentable struct wrapper -- confirmed by
// existing use elsewhere (ContentView.swift's `selectedMode ==
// AkzStretchMode_Cyclic`, PresetStore.swift's `AkzMachine(rawValue:)`).
// So Equatable synthesizes here for free; no hand-written `==` needed.

import AkaizerCore

public struct ParamSnapshot: Equatable {
    public var machine: AkzMachine
    public var engine: AkzEngine
    public var mode: AkzStretchMode
    public var stretchPercent: Double
    public var cycleLength: Double
    public var quality: Double
    public var width: Double
    public var transposeSemitones: Double
    public var filterCutoff: Double
    public var filterResonance: Double
    /// The bandwidth knob's rate, in Hz -- always a real, in-range value
    /// (2.1: never 0/bypass, see AkzStretchParams.sampleRateHz and
    /// RateModel.h). No default argument on purpose: every call site
    /// naming an explicit value is what stops a future accidental
    /// sentinel from compiling.
    public var sampleRateHz: Double

    public init(
        machine: AkzMachine, engine: AkzEngine, mode: AkzStretchMode,
        stretchPercent: Double, cycleLength: Double, quality: Double, width: Double,
        transposeSemitones: Double, filterCutoff: Double, filterResonance: Double,
        sampleRateHz: Double
    ) {
        self.machine = machine
        self.engine = engine
        self.mode = mode
        self.stretchPercent = stretchPercent
        self.cycleLength = cycleLength
        self.quality = quality
        self.width = width
        self.transposeSemitones = transposeSemitones
        self.filterCutoff = filterCutoff
        self.filterResonance = filterResonance
        self.sampleRateHz = sampleRateHz
    }

    public init(params: AkzStretchParams) {
        machine = params.machine
        engine = params.engine
        mode = params.mode
        stretchPercent = Double(params.timeFactorPercent)
        cycleLength = Double(params.cycleLengthSamples)
        quality = Double(params.quality)
        width = Double(params.width)
        transposeSemitones = Double(params.transposeSemitones)
        filterCutoff = Double(params.filterCutoff01)
        filterResonance = Double(params.filterResonance01)
        sampleRateHz = Double(params.sampleRateHz)
    }

    public var params: AkzStretchParams {
        AkzStretchParams(
            machine: machine, engine: engine, mode: mode,
            timeFactorPercent: Float(stretchPercent),
            cycleLengthSamples: Int32(cycleLength),
            quality: Int32(quality),
            width: Int32(width),
            transposeSemitones: Float(transposeSemitones),
            filterCutoff01: Float(filterCutoff),
            filterResonance01: Float(filterResonance),
            sampleRateHz: Float(sampleRateHz)
        )
    }

    /// Wraps StretchProcessor.defaultParams(machine:) -- that stays the
    /// single source of machine defaults; this is just its Equatable-
    /// comparable mirror.
    public static func defaults(for machine: AkzMachine) -> ParamSnapshot {
        ParamSnapshot(params: StretchProcessor.defaultParams(machine: machine))
    }
}

public extension AkaizerPreset {
    /// Bridge only -- AkaizerPreset's own Codable representation (its
    /// per-field raw-value encoding) is the on-disk JSON format and must
    /// not change just because ParamSnapshot exists.
    var snapshot: ParamSnapshot { ParamSnapshot(params: params) }
}
