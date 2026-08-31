// ProcessedWavExport.swift
//
// Transferable value behind WaveformView's .draggable() (see
// ContentView's _dragExport(for:)) -- dragging the waveform out of the
// window hands Finder/a DAW a .wav of the PROCESSED result, rendering it
// first if processedChannels is missing or stale (see ContentView's
// _renderIsStale). FileRepresentation is a file promise under the hood
// (the same AppKit machinery NSFilePromiseProvider wraps by hand) --
// using it here instead of NSFilePromiseProvider directly is ~15 lines
// against an NSViewRepresentable + NSView subclass + delegate + queue,
// and neither of NSFilePromiseProvider's real reasons to exist (a custom
// drag image, pre-macOS-13 support) applies to this app (.macOS(.v14)).
//
// The autoclosure that builds this value runs at drag START, so it must
// be cheap -- everything it captures (LoadedSample, a ~70-byte
// ParamSnapshot, an already-rendered [[Float]] when available) is data,
// never work. FileRepresentation's exporting closure below only runs
// once the drop is ACCEPTED, and is async -- so a cold drag (no render
// yet, or a stale one) never stalls the drag gesture itself; the render
// happens after the drop, exactly where Finder's own promise UI expects
// a moment's wait on a large file.
//
// Always exports .wav regardless of the source's container (AIFF
// included), per the feedback's own wording -- safe by construction,
// since PCMConversion/AiffCodec/WavCodec all normalise to little-endian
// internally (see AiffCodec.swift's read/write), so an AIFF source's
// bytes carry over losslessly into a WAV container. This is
// deliberately NOT the same rule saveProcessed()'s Save panel uses
// (format follows the source there) -- an explicit Save As should match
// what you opened; a drag to a DAW wants the universal answer.

import AkaizerAudio
import AkaizerCore
import CoreTransferable
import Foundation
import UniformTypeIdentifiers

struct ProcessedWavExport: Transferable {
    let source: LoadedSample
    let snapshot: ParamSnapshot
    /// Already-rendered channels matching `snapshot`, if any -- nil means
    /// "render fresh," which _materialize() below does on demand.
    let cachedChannels: [[Float]]?
    let fileName: String
    /// Called once, on the main actor, only when a fresh render actually
    /// happened -- lets ContentView adopt it as the new processedChannels
    /// (a drag-out of a stale state doubles as a Process) without this
    /// type needing to know anything about ContentView's @State.
    let onRendered: (@Sendable ([[Float]], ParamSnapshot) -> Void)?

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .wav) { item in
            SentTransferredFile(try await item._materialize())
        }
    }

    private func _materialize() async throws -> URL {
        let channels: [[Float]]
        if let cachedChannels {
            channels = cachedChannels
        } else {
            let source = self.source
            let params = snapshot.params
            channels = await Task.detached(priority: .userInitiated) {
                ProcessedRender.render(sample: source, params: params)
            }.value
            let rendered = channels
            let snap = snapshot
            await MainActor.run { onRendered?(rendered, snap) }
        }

        // A per-drag UUID *directory* (not a UUID filename, unlike
        // verifyRoundTrip's temp file) is what lets the dropped file
        // carry the sample's own readable name rather than a GUID.
        // Nothing calls back when the receiver has finished copying the
        // promised file, so cleanup is a sweep on next launch (see
        // ContentView's .onAppear) rather than a delete-on-completion.
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("Patina-Drag", isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent(fileName)
        try ProcessedRender.writeWav(channels: channels, format: source.format, to: url)
        return url
    }
}
