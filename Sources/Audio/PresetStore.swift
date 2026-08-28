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

import AkaizerCore
import Foundation

public struct AkaizerPreset: Codable, Identifiable, Equatable {
    public var id: String { name }
    public var name: String

    private var machineRawValue: UInt32
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
        machineRawValue = params.machine.rawValue
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

    /// Reconstructs full params. This toolchain's imported C enums have
    /// a non-failable `init(rawValue:)` (it wraps any UInt32 directly,
    /// unlike a Swift-native RawRepresentable enum), so an unrecognised
    /// raw value -- e.g. a preset saved by a future version with a
    /// machine this build doesn't know about -- produces a garbage-but-
    /// non-crashing enum instance rather than needing a fallback here.
    public var params: AkzStretchParams {
        AkzStretchParams(
            machine: AkzMachine(rawValue: machineRawValue),
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
    public init(baseDirectory: URL) {
        let dir = baseDirectory.appendingPathComponent("AkaizerS", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        fileURL = dir.appendingPathComponent("presets.json")
    }

    public func load() -> [AkaizerPreset] {
        guard let data = try? Data(contentsOf: fileURL) else { return [] }
        return (try? JSONDecoder().decode([AkaizerPreset].self, from: data)) ?? []
    }

    public func save(_ presets: [AkaizerPreset]) {
        guard let data = try? JSONEncoder().encode(presets) else { return }
        try? data.write(to: fileURL, options: .atomic)
    }
}
