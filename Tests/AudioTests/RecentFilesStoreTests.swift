// RecentFilesStoreTests.swift
//
// v2 heritage-roster plan, stage 1: recent files persistence (v1 shipped
// this session-only). Uses RecentFilesStore(baseDirectory:) to point at
// a temp directory, mirroring PresetStoreTests.

import Foundation
import XCTest

@testable import AkaizerAudio

final class RecentFilesStoreTests: XCTestCase {
    private func makeTempStore(maxCount: Int = 8) -> RecentFilesStore {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        return RecentFilesStore(baseDirectory: dir, maxCount: maxCount)
    }

    private func makeExistingFile() -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".wav")
        FileManager.default.createFile(atPath: url.path, contents: Data())
        return url
    }

    func testLoadOnFreshStoreIsEmpty() {
        XCTAssertEqual(makeTempStore().load(), [])
    }

    func testSaveThenLoadRoundTripsOrder() {
        let store = makeTempStore()
        let urls = (0..<3).map { _ in makeExistingFile() }
        store.save(urls)
        XCTAssertEqual(store.load(), urls)
    }

    func testLoadDropsPathsThatNoLongerExist() {
        let store = makeTempStore()
        let stillThere = makeExistingFile()
        let goneURL = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".wav")
        store.save([goneURL, stillThere])

        XCTAssertEqual(store.load(), [stillThere])
    }

    func testSaveTruncatesToMaxCount() {
        let store = makeTempStore(maxCount: 2)
        let urls = (0..<5).map { _ in makeExistingFile() }
        store.save(urls)

        XCTAssertEqual(store.load(), Array(urls.prefix(2)))
    }

    func testCorruptFileLoadsAsEmptyRatherThanCrashing() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let store = RecentFilesStore(baseDirectory: dir)
        let fileURL = dir.appendingPathComponent("Patina", isDirectory: true).appendingPathComponent("recents.json")
        try "not json".write(to: fileURL, atomically: true, encoding: .utf8)

        XCTAssertEqual(store.load(), [])
    }
}
