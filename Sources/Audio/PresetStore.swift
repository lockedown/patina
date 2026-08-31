// PresetStore.swift
//
// Build order stage 9: named parameter sets ("Jungle S950", "Dusty
// S1000" -- the plan's own examples). A preset is just a snapshot of
// AkzStretchParams; the Codable wrapper stores each C enum field's raw
// value rather than the enum itself, since the imported C enums aren't
// Codable directly, and converts back via the failable `init(rawValue:)`
// the Clang importer synthesizes.
//
// Persisted as one JSON file in Application Support -- no database, no
// UserDefaults blob, just a small array a person could open and read if
// they wanted to. Matches the project's zero-dependency stance: this is
// Foundation's JSONEncoder/Decoder, nothing more.
//
// v2 format change (heritage-roster plan, stage 1): machine identity
// moved from AkzMachine's raw UInt32 to AkzMachineProfile.stableId, a
// string that never changes once a machine ships. v1 stored the raw
// enum value directly -- fine while the roster was fixed at six Akai
// machines in a fixed order, but v2 both reorders nothing (AkzMachine is
// still append-only, never renumbered) AND makes the raw value alone an
// unnecessary, permanent liability: a decoder bug or a future refactor
// that ever DID reorder the enum would silently remap every saved
// preset to a different machine with no way to detect it. The stable id
// is what removes that risk permanently, not just for this roster
// expansion.

import AkaizerCore
import Foundation

/// Maps a v1 preset's raw AkzMachine value to this format's stable id.
/// FROZEN FOREVER the moment this ships -- never edit, insert into, or
/// remove from this table, even if AkzMachine itself is ever
/// restructured. This is the one place that makes "AkzMachine must never
/// be renumbered" survivable if it's ever violated by mistake: as long
/// as this table still maps 0..5 to the six Akai ids below, an old
/// preset keeps naming the right machine regardless of what the enum
/// looks like by the time it's loaded.
enum PresetMigration {
    static let v1RawValueToStableId: [UInt32: String] = [
        0: "akai.s900",
        1: "akai.s950",
        2: "akai.s1000",
        3: "akai.s2000",
        4: "akai.s3000",
        5: "akai.s3200",
    ]

    /// Used only if a v1 file names a raw value outside 0...5, which
    /// should not be possible (v1 only ever wrote its own six machines'
    /// raw values) but is handled rather than trusted.
    static let fallbackStableId = "akai.s950"
}

public struct AkaizerPreset: Codable, Identifiable, Equatable {
    /// Bump only if a future change can't be made additive via
    /// `decodeIfPresent` alone (has not been needed since v1 -> v2).
    public static let currentFormatVersion = 2

    public var id: String { name }
    public var name: String
    public var formatVersion: Int
    public var machineId: String

    private var engineRawValue: UInt32
    private var modeRawValue: UInt32
    private var timeFactorPercent: Float
    private var cycleLengthSamples: Int32
    private var quality: Int32
    private var width: Int32
    private var transposeSemitones: Float
    private var filterCutoff01: Float
    private var filterResonance01: Float

    public init(name: String, params: AkzStretchParams) {
        self.name = name
        formatVersion = Self.currentFormatVersion
        machineId = StretchProcessor.profile(for: params.machine).stableIdString
        engineRawValue = params.engine.rawValue
        modeRawValue = params.mode.rawValue
        timeFactorPercent = params.timeFactorPercent
        cycleLengthSamples = params.cycleLengthSamples
        quality = params.quality
        width = params.width
        transposeSemitones = params.transposeSemitones
        filterCutoff01 = params.filterCutoff01
        filterResonance01 = params.filterResonance01
    }

    private enum CodingKeys: String, CodingKey {
        case name, formatVersion, machineId
        case machineRawValue // v1 only; absent from every v2+ file
        case engineRawValue, modeRawValue
        case timeFactorPercent, cycleLengthSamples, quality, width
        case transposeSemitones, filterCutoff01, filterResonance01
    }

    /// Hand-written rather than synthesised specifically so `machineId`
    /// (and any field added after it) can be read with
    /// `decodeIfPresent` -- see the header comment: this is what makes
    /// every future field additive, with no v3 format needed just to add
    /// one knob's worth of persisted state.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        // Not read back -- always normalised to currentFormatVersion on
        // load, since load() always re-saves through encode(to:) below
        // the moment a v1 file is touched. Kept as a field (not derived)
        // so the on-disk JSON is self-describing to a human reader.
        formatVersion = Self.currentFormatVersion

        if let machineId = try c.decodeIfPresent(String.self, forKey: .machineId) {
            self.machineId = machineId
        } else {
            let raw = try c.decode(UInt32.self, forKey: .machineRawValue)
            self.machineId = PresetMigration.v1RawValueToStableId[raw] ?? PresetMigration.fallbackStableId
        }

        engineRawValue = try c.decode(UInt32.self, forKey: .engineRawValue)
        modeRawValue = try c.decode(UInt32.self, forKey: .modeRawValue)
        timeFactorPercent = try c.decode(Float.self, forKey: .timeFactorPercent)
        cycleLengthSamples = try c.decode(Int32.self, forKey: .cycleLengthSamples)
        quality = try c.decode(Int32.self, forKey: .quality)
        width = try c.decode(Int32.self, forKey: .width)
        transposeSemitones = try c.decode(Float.self, forKey: .transposeSemitones)
        filterCutoff01 = try c.decode(Float.self, forKey: .filterCutoff01)
        filterResonance01 = try c.decode(Float.self, forKey: .filterResonance01)
    }

    /// Always writes the current format -- a v1 file loaded and re-saved
    /// (e.g. the user renames one preset) silently upgrades the whole
    /// array, matching PresetStore.loadOrRecover()'s "re-save once" note.
    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(name, forKey: .name)
        try c.encode(Self.currentFormatVersion, forKey: .formatVersion)
        try c.encode(machineId, forKey: .machineId)
        try c.encode(engineRawValue, forKey: .engineRawValue)
        try c.encode(modeRawValue, forKey: .modeRawValue)
        try c.encode(timeFactorPercent, forKey: .timeFactorPercent)
        try c.encode(cycleLengthSamples, forKey: .cycleLengthSamples)
        try c.encode(quality, forKey: .quality)
        try c.encode(width, forKey: .width)
        try c.encode(transposeSemitones, forKey: .transposeSemitones)
        try c.encode(filterCutoff01, forKey: .filterCutoff01)
        try c.encode(filterResonance01, forKey: .filterResonance01)
    }

    /// Reconstructs full params. `StretchProcessor.machine(forStableId:)`
    /// falls back to S950 for an id this build doesn't recognise (e.g. a
    /// preset saved by a future version naming a machine not yet added
    /// here) rather than crashing or producing a garbage enum value --
    /// see that function's doc comment.
    public var params: AkzStretchParams {
        AkzStretchParams(
            machine: StretchProcessor.machine(forStableId: machineId),
            engine: AkzEngine(rawValue: engineRawValue),
            mode: AkzStretchMode(rawValue: modeRawValue),
            timeFactorPercent: timeFactorPercent,
            cycleLengthSamples: cycleLengthSamples,
            quality: quality,
            width: width,
            transposeSemitones: transposeSemitones,
            filterCutoff01: filterCutoff01,
            filterResonance01: filterResonance01
        )
    }
}

public final class PresetStore {
    private let fileURL: URL

    public convenience init() {
        let baseDir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        self.init(baseDirectory: baseDir)
    }

    /// For tests: points at an arbitrary directory (e.g. a temp
    /// directory) instead of the real app's Application Support, so
    /// exercising save/load doesn't touch -- or clobber -- an actual
    /// user's saved presets.
    ///
    /// Also does the one-time "AkaizerS" -> "Patina" directory migration
    /// (the app's v2 rename): if the new directory has no presets.json
    /// yet and the old one does, COPY (not move) the old file across.
    /// Copying rather than moving means a rollback to a pre-rename build
    /// still finds its presets untouched.
    public init(baseDirectory: URL) {
        let dir = baseDirectory.appendingPathComponent("Patina", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        fileURL = dir.appendingPathComponent("presets.json")

        let legacyFileURL = baseDirectory
            .appendingPathComponent("AkaizerS", isDirectory: true)
            .appendingPathComponent("presets.json")
        if !FileManager.default.fileExists(atPath: fileURL.path),
           FileManager.default.fileExists(atPath: legacyFileURL.path) {
            try? FileManager.default.copyItem(at: legacyFileURL, to: fileURL)
        }
    }

    /// Loads presets, tolerating and recovering from a corrupt or
    /// unreadable file instead of silently discarding it. `error` is
    /// non-nil only when something needs surfacing to the user -- a
    /// missing file (fresh install) is not an error, it's `([], nil)`.
    ///
    /// v1's `load()` returned `[]` on any decode failure with no signal
    /// at all -- not just unfriendly, actively dangerous: the very next
    /// `save(_:)` call (e.g. saving a new preset) would then overwrite
    /// the corrupt-but-possibly-recoverable file with a genuinely empty
    /// array, turning a readable-by-hand corruption into unrecoverable
    /// data loss. This preserves the original under a `.corrupt-<epoch>`
    /// name before returning an empty list, so the failure is at worst
    /// inconvenient, never silent.
    public func loadOrRecover() -> (presets: [AkaizerPreset], error: String?) {
        guard let data = try? Data(contentsOf: fileURL) else {
            return ([], nil)
        }
        do {
            let presets = try JSONDecoder().decode([AkaizerPreset].self, from: data)
            return (presets, nil)
        } catch {
            let corruptURL = fileURL.deletingLastPathComponent()
                .appendingPathComponent("presets.json.corrupt-\(Int(Date().timeIntervalSince1970))")
            try? FileManager.default.copyItem(at: fileURL, to: corruptURL)
            return ([], "Presets file was unreadable and has been preserved as \(corruptURL.lastPathComponent).")
        }
    }

    public func save(_ presets: [AkaizerPreset]) {
        guard let data = try? JSONEncoder().encode(presets) else { return }
        try? data.write(to: fileURL, options: .atomic)
    }
}
