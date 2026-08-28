// PresetStoreTests.swift
//
// Build order stage 9. Uses PresetStore(baseDirectory:) to point at a
// temp directory rather than the real app's Application Support, so
// running this suite never touches -- or clobbers -- an actual user's
// saved presets.

import Foundation
import XCTest

@testable import AkaizerAudio
import AkaizerCore

final class PresetStoreTests: XCTestCase {
    private func makeTempStore() -> PresetStore {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        return PresetStore(baseDirectory: dir)
    }

    func testLoadOnFreshStoreIsEmpty() {
        XCTAssertEqual(makeTempStore().load().count, 0)
    }

    func testSaveThenLoadRoundTripsEveryField() {
        var params = StretchProcessor.defaultParams(machine: AkzMachine_S2000)
        params.engine = AkzEngine_Revised
        params.mode = AkzStretchMode_Intelligent
        params.timeFactorPercent = 137.5
        params.cycleLengthSamples = 842
        params.quality = 77
        params.width = 33
        params.transposeSemitones = -7
        params.filterCutoff01 = 0.42
        params.filterResonance01 = 0.91

        let preset = AkaizerPreset(name: "Jungle S950", params: params)
        let store = makeTempStore()
        store.save([preset])

        let loaded = store.load()
        XCTAssertEqual(loaded.count, 1)
        let roundTripped = loaded[0].params

        XCTAssertEqual(roundTripped.machine.rawValue, AkzMachine_S2000.rawValue)
        XCTAssertEqual(roundTripped.engine.rawValue, AkzEngine_Revised.rawValue)
        XCTAssertEqual(roundTripped.mode.rawValue, AkzStretchMode_Intelligent.rawValue)
        XCTAssertEqual(roundTripped.timeFactorPercent, 137.5)
        XCTAssertEqual(roundTripped.cycleLengthSamples, 842)
        XCTAssertEqual(roundTripped.quality, 77)
        XCTAssertEqual(roundTripped.width, 33)
        XCTAssertEqual(roundTripped.transposeSemitones, -7)
        XCTAssertEqual(roundTripped.filterCutoff01, 0.42, accuracy: 1e-6)
        XCTAssertEqual(roundTripped.filterResonance01, 0.91, accuracy: 1e-6)
        XCTAssertEqual(loaded[0].name, "Jungle S950")
    }

    func testSaveOverwritesAcrossSeparateStoreInstances() {
        // Simulates the app's actual usage: load, mutate, save, and a
        // later launch (a fresh PresetStore instance) sees the update --
        // not just "the same object remembers what I set."
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let storeA = PresetStore(baseDirectory: dir)

        let params = StretchProcessor.defaultParams(machine: AkzMachine_S1000)
        storeA.save([AkaizerPreset(name: "Dusty S1000", params: params)])

        let storeB = PresetStore(baseDirectory: dir)
        let loaded = storeB.load()
        XCTAssertEqual(loaded.count, 1)
        XCTAssertEqual(loaded[0].name, "Dusty S1000")
    }

    func testMultiplePresetsPreserveOrder() {
        let store = makeTempStore()
        let params = StretchProcessor.defaultParams(machine: AkzMachine_S950)
        let names = ["Alpha", "Beta", "Gamma"]
        store.save(names.map { AkaizerPreset(name: $0, params: params) })

        XCTAssertEqual(store.load().map { $0.name }, names)
    }
}
