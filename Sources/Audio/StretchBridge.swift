// StretchBridge.swift
//
// Thin Swift wrapper around the C API in AkaizerCore/include/AkaizerCore.h.
// Deliberately thin: the C types (AkzMachine, AkzEngine, AkzStretchMode,
// AkzStretchParams) are used directly rather than re-declared as parallel
// Swift enums, so the machine profile table in MachineProfile.cpp stays
// the single source of truth (plan: "reuse existing").

import AkaizerCore

/// Owns one AkzStretchEngine handle -- the synchronous, offline engine.
/// NOT safe to drive from an audio render thread: process() recomputes
/// (allocating) on whatever thread calls it when params/source changed.
/// Used for the app's offline Process/Save path. For live audition from
/// an AVAudioSourceNode render callback, use RealtimePlayer below instead.
public final class StretchProcessor {
    private var engine: OpaquePointer?

    public init(sampleRateHz: Double) {
        engine = akz_stretch_engine_create(sampleRateHz)
    }

    deinit {
        akz_stretch_engine_destroy(engine)
    }

    public func reset() {
        akz_stretch_engine_reset(engine)
    }

    public func setParams(_ params: AkzStretchParams) {
        var p = params
        akz_stretch_engine_set_params(engine, &p)
    }

    public func setSource(_ samples: [Float]) {
        akz_stretch_engine_set_source(engine, samples, samples.count)
    }

    public var outputLength: Int {
        Int(akz_stretch_engine_output_length(engine))
    }

    /// Renders the entire output in one call -- fine for the offline
    /// milestone this is currently used for. See StretchEngine.h's
    /// stage-4 TODO for why this isn't yet how real-time audition will
    /// pull audio.
    public func renderAll() -> [Float] {
        let len = outputLength
        guard len > 0 else { return [] }
        var out = [Float](repeating: 0, count: len)
        let written = out.withUnsafeMutableBufferPointer { buf in
            akz_stretch_engine_process(engine, buf.baseAddress, len)
        }
        if written < len {
            out.removeLast(len - written)
        }
        return out
    }

    public static func defaultParams(machine: AkzMachine) -> AkzStretchParams {
        var params = AkzStretchParams(
            machine: machine, engine: AkzEngine_Classic, mode: AkzStretchMode_Cyclic,
            timeFactorPercent: 100, cycleLengthSamples: 1000, quality: 10, width: 10,
            transposeSemitones: 0, filterCutoff01: 1, filterResonance01: 0,
            sampleRateHz: 0 // 0 = machine default -- see AkaizerCore.h
        )
        akz_stretch_params_default(machine, &params)
        return params
    }

    public static func profile(for machine: AkzMachine) -> AkzMachineProfile {
        akz_machine_profile(machine).pointee
    }

    /// All machines in declaration order, for building the roster
    /// browser. Derived from akz_machine_count() (v2 heritage-roster
    /// plan, stage 9) rather than a hand-maintained literal array --
    /// adding a machine to the C enum is now enough on its own; nothing
    /// on the Swift side has to be kept in sync by hand.
    public static let allMachines: [AkzMachine] = (0..<akz_machine_count()).map { AkzMachine(rawValue: UInt32($0)) }

    /// Reverse lookup for AkzMachineProfile.stableId -- what
    /// AkaizerPreset.machineId stores on disk. Linear scan over six (soon
    /// a dozen-plus) machines, called only on preset load/apply, never on
    /// a hot path. An unrecognised id (a preset from a future build
    /// naming a machine this one doesn't have) falls back to S950 rather
    /// than crashing or producing a garbage enum -- see AkaizerPreset's
    /// doc comment for why S950 specifically.
    public static func machine(forStableId stableId: String) -> AkzMachine {
        for machine in allMachines where profile(for: machine).stableIdString == stableId {
            return machine
        }
        return AkzMachine_S950
    }

    /// The stages provenance is tracked for, in AkzStage's own order --
    /// what the UI iterates to build a "modelled from..." panel.
    public static let allStages: [AkzStage] = [
        AkzStage_Rate, AkzStage_Converter, AkzStage_Filter,
        AkzStage_Interpolator, AkzStage_Stretch, AkzStage_Dac,
    ]

    /// Human-readable label for one stage, for provenance UI.
    public static func label(for stage: AkzStage) -> String {
        switch stage {
        case AkzStage_Rate: return "Sample rate"
        case AkzStage_Converter: return "Converter"
        case AkzStage_Filter: return "Filter"
        case AkzStage_Interpolator: return "Interpolator"
        case AkzStage_Stretch: return "Time-stretch"
        case AkzStage_Dac: return "DAC"
        default: return "?"
        }
    }

    /// Provenance for one machine/stage pair -- see AkaizerCore.h's
    /// AkzStageProvenance. Never nil in practice (the C side guarantees
    /// an entry for every pair, enforced by a completeness test), but
    /// the pointer is still checked since it crosses the C boundary.
    public static func provenance(for machine: AkzMachine, stage: AkzStage) -> (level: AkzProvenanceLevel, note: String) {
        guard let entry = akz_machine_stage_provenance(machine, stage) else {
            return (AkzProvenanceLevel_Unmodelled, "?")
        }
        let note = entry.pointee.note != nil ? String(cString: entry.pointee.note) : "?"
        return (entry.pointee.level, note)
    }
}

/// Owns one AkzRealtimePlayer -- the render-thread-safe player used for
/// live audition. setSource()/setParams() are main-thread/UI-thread
/// calls (they may allocate); pull() is safe to call from a CoreAudio
/// render callback (see AkaizerCore.h's "Real-time audition player"
/// section for the full contract).
public final class RealtimePlayer {
    private var player: OpaquePointer?

    public init(sampleRateHz: Double) {
        player = akz_realtime_player_create(sampleRateHz)
    }

    deinit {
        akz_realtime_player_destroy(player)
    }

    /// Main-thread only.
    public func setSource(_ samples: [Float]) {
        akz_realtime_player_set_source(player, samples, samples.count)
    }

    /// Main-thread only.
    public func setParams(_ params: AkzStretchParams) {
        var p = params
        akz_realtime_player_set_params(player, &p)
    }

    /// Render-thread safe. Fills `frameCount` frames (looping the most
    /// recently published render, or silence before the first one).
    public func pull(frameCount: Int, into buffer: UnsafeMutablePointer<Float>) {
        akz_realtime_player_pull(player, buffer, frameCount)
    }

    /// Render-thread safe, non-blocking.
    public var isReady: Bool {
        akz_realtime_player_is_ready(player) != 0
    }

    /// Render-thread safe, non-blocking. See AkaizerCore.h's comment on
    /// akz_realtime_player_has_pending_commit -- true once a
    /// stretch-affecting re-render is waiting to be swapped in. Must be
    /// checked on every channel's player before calling commitPending()
    /// on any of them; see LiveAuditionController's render callback.
    public var hasPendingCommit: Bool {
        akz_realtime_player_has_pending_commit(player) != 0
    }

    /// Render-thread safe. No-op if hasPendingCommit was false.
    public func commitPending() {
        akz_realtime_player_commit_pending(player)
    }

    /// Render-thread safe, non-blocking. True only while the worker is
    /// actively re-rendering -- unlike isReady above, this genuinely
    /// toggles back to false, so it's the one that can drive a
    /// "recomputing" UI indicator.
    public var isRecomputing: Bool {
        akz_realtime_player_is_recomputing(player) != 0
    }
}

public extension AkzMachineProfile {
    /// `name` is a `const char*` into static C storage -- always valid,
    /// never needs freeing (see AkaizerCore.h), so this is a safe, cheap
    /// conversion.
    var displayName: String {
        name != nil ? String(cString: name) : "?"
    }

    /// Same storage/lifetime guarantee as `name` above. What
    /// AkaizerPreset.machineId compares against.
    var stableIdString: String {
        stableId != nil ? String(cString: stableId) : "?"
    }

    /// Same storage/lifetime guarantee as `name` above. Roster-browser
    /// grouping (v2 heritage-roster plan, stage 9) -- display only.
    var manufacturerName: String {
        manufacturer != nil ? String(cString: manufacturer) : "?"
    }
}
