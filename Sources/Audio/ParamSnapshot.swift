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

// Explicit Sendable conformance (2.3.2): every stored property is a plain
// Double or a Clang-imported C enum wrapper, both genuinely Sendable --
// the compiler just can't infer that across the AkaizerCore module
// boundary without @preconcurrency on every importer, so it's declared
// here instead. Needed once ProcessedWavExport.swift started passing a
// ParamSnapshot into a @Sendable closure (the drag-export fix that
// stopped awaiting the MainActor hop -- see that file's comment).
public struct ParamSnapshot: Equatable, Sendable {
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

public extension ParamSnapshot {
    /// One canonical ParamID -> stored-value correspondence, so anything
    /// walking MachineControls' descriptors (adapted(to:) below) doesn't
    /// carry its own copy of it. Exhaustive on purpose: a new ParamID
    /// case won't compile until it's mapped here. ContentView's
    /// `_binding(for:)` still maps the same IDs onto its @State vars --
    /// that's a mapping to STORAGE, not to a value, and can't move here
    /// without the SamplerModel extraction the v2 plan skipped.
    subscript(id: ParamID) -> Double {
        get {
            switch id {
            case .transpose: return transposeSemitones
            case .bandwidth: return sampleRateHz
            case .cutoff: return filterCutoff
            case .resonance: return filterResonance
            case .stretch: return stretchPercent
            case .cycle: return cycleLength
            case .quality: return quality
            case .width: return width
            }
        }
        set {
            switch id {
            case .transpose: transposeSemitones = newValue
            case .bandwidth: sampleRateHz = newValue
            case .cutoff: filterCutoff = newValue
            case .resonance: filterResonance = newValue
            case .stretch: stretchPercent = newValue
            case .cycle: cycleLength = newValue
            case .quality: quality = newValue
            case .width: width = newValue
            }
        }
    }

    /// The same settings, carried onto `machine` -- 2.4 change request:
    /// "maintain knob settings between emulators... allow same sample
    /// with same settings to stay in place while flipping between
    /// emulators." Replaces the reset-to-that-machine's-defaults
    /// behaviour _selectMachine used to have. Two rules:
    ///
    /// 1. A knob the target machine actually SHOWS (per
    ///    MachineControls.controls(for:mode:)) is clamped into that
    ///    knob's own range. An S1000 at 1500% arriving on an S950 (max
    ///    999%) must read 999 -- lossy on purpose: a knob displaying a
    ///    value outside its own travel, that the DSP then clamps
    ///    anyway, is the illegibility 2.1 fixed for bandwidth.
    /// 2. A knob the target machine does NOT show is left completely
    ///    alone -- not reset, not clamped. Nothing can see or hear it
    ///    while parked: the knob row, the LCD rows and the DSP are all
    ///    gated on the same capability flags (StretchEngine.cpp's
    ///    supportsTimeStretch and effectiveMode gates, FilterModel.h's
    ///    resonance gate, RateModel.h's fixed-rate collapse). The
    ///    payoff is a lossless round trip: S950 at 48kHz -> SP-1200 ->
    ///    S950 gives 48kHz back exactly, where clamping would have
    ///    quietly rewritten it to that machine's fixed 26.04kHz.
    ///
    /// `mode` and `engine` ride through untouched. Mode used to be
    /// forced to CYCLIC on a machine with no mode switch; that only
    /// threw away an S1000 user's INTELLIGENT selection on the way
    /// through an S950, since every consumer already ignores it without
    /// the switch (ContentView._isIntelligentMode, controls(for:mode:),
    /// StretchEngine's effectiveMode). Leaving it alone also means the
    /// mode passed to controls() below IS the result's own mode -- no
    /// chicken-and-egg, and the transform is idempotent.
    ///
    /// Descriptor-driven rather than hand-clamping the two ranges that
    /// actually vary today (bandwidth, stretch): the visible set and the
    /// ranges come from one place, so a future profile whose cutoff or
    /// cycle range narrows is handled by construction. Cannot build an
    /// invalid range -- it only ever READS descriptor.range, and the
    /// `25...maxStretchPercent` descriptor is only emitted when
    /// supportsTimeStretch != 0 (see MachineControls.swift's own note on
    /// why there's no max(25.0, ...) there).
    func adapted(to machine: AkzMachine) -> ParamSnapshot {
        var result = self
        result.machine = machine
        for descriptor in MachineControls.controls(for: machine, mode: mode) {
            let range = descriptor.range
            result[descriptor.id] = min(max(result[descriptor.id], range.lowerBound), range.upperBound)
        }
        return result
    }
}
