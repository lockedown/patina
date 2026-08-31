// RecentFilesStore.swift
//
// Persists the sidebar's Recent Files list. v1 shipped this
// session-only ("Session-only for now; stage 9 (presets) may give this
// real persistence" -- v1's own comment, and README's Known limitations)
// -- this is that persistence, arriving in v2 instead.
//
// Mirrors PresetStore's shape on purpose (injectable baseDirectory for
// tests, atomic save, tolerant load) rather than introducing a second
// persistence pattern for one more small JSON file.
//
// Unsandboxed, so a plain path String round-trips fine: FileManager can
// re-open any path the user has permission to just by having opened it
// once before, no bookmark needed. If this app is ever sandboxed, the
// stored payload would need to become security-scoped bookmark `Data`
// instead of a path String, and loading a recent file would need
// `url.startAccessingSecurityScopedResource()` around the load -- the
// store's own shape (an ordered array, atomic save/load, tolerant of a
// missing/corrupt file) would not need to change.

import Foundation

private struct RecentFilesPayload: Codable {
    var formatVersion: Int
    var paths: [String]
}

public final class RecentFilesStore {
    private let fileURL: URL
    private let maxCount: Int

    public convenience init(maxCount: Int = 8) {
        let baseDir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        self.init(baseDirectory: baseDir, maxCount: maxCount)
    }

    /// For tests: an arbitrary directory instead of the real app's
    /// Application Support.
    public init(baseDirectory: URL, maxCount: Int = 8) {
        let dir = baseDirectory.appendingPathComponent("Patina", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        fileURL = dir.appendingPathComponent("recents.json")
        self.maxCount = maxCount
    }

    /// Loads the list, silently dropping any path whose file no longer
    /// exists at that location -- a moved or deleted sample should just
    /// vanish from Recent, not surface as an error nobody asked to see.
    /// A missing or corrupt file (fresh install, or a hand-edited file
    /// gone wrong) is treated the same way: an empty list, not an error
    /// -- Recent Files carries no data worth a corrupt-file recovery
    /// path the way presets.json does.
    public func load() -> [URL] {
        guard let data = try? Data(contentsOf: fileURL),
              let payload = try? JSONDecoder().decode(RecentFilesPayload.self, from: data) else {
            return []
        }
        return payload.paths
            .map { URL(fileURLWithPath: $0) }
            .filter { FileManager.default.fileExists(atPath: $0.path) }
    }

    /// Truncates to `maxCount` on write -- the caller (ContentView's
    /// `_addToRecentFiles`) already enforces this in memory, but the
    /// store enforces its own on-disk invariant rather than trusting the
    /// caller never to change.
    public func save(_ urls: [URL]) {
        let payload = RecentFilesPayload(formatVersion: 1, paths: Array(urls.prefix(maxCount)).map(\.path))
        guard let data = try? JSONEncoder().encode(payload) else { return }
        try? data.write(to: fileURL, options: .atomic)
    }
}
