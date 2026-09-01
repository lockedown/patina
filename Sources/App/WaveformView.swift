// WaveformView.swift
//
// Build order stage 9: source and processed waveforms overlaid,
// phosphor-green -- matching LCDReadoutView's fixed retro palette (see
// that file's design rationale; this shares it deliberately, as one
// visual family). Original renders dim underneath; processed renders
// bright on top, so the shape of the edit is visible at a glance.
//
// 2.1 feedback ("show playback bar over sample waveform... click and
// move start point with mouse") made this genuinely interactive: a
// playhead bar tracks whichever source is currently playing, and
// clicking/dragging the waveform moves the start point playback and
// rendering trim from. Stays a dumb view -- no @State, no knowledge of
// ContentView's playback controllers -- everything it needs (traces,
// fractions, callbacks) is passed in, same as before.

import AkaizerAudio
import SwiftUI

struct WaveformView: View {
    /// Mono peak source, already deinterleaved to one channel (e.g.
    /// channel 0) -- the waveform's shape doesn't need every channel to
    /// be a useful visual reference.
    let samples: [Float]
    let overlaySamples: [Float]?

    /// [0, 1] position of the start point, in the SAME shared coordinate
    /// space _drawTrace lays both traces out in (see referenceCount
    /// below) -- not "fraction of samples.count," which would drift out
    /// of alignment with the trace itself whenever overlaySamples is a
    /// different length.
    let startFraction: Double
    /// [0, 1] playback position in that same shared space, or nil when
    /// nothing is playing. Converting whichever buffer is actually
    /// playing (which may be samples, overlaySamples, or a trimmed
    /// version of either) into this shared fraction is ContentView's
    /// job, not this view's -- it only draws where it's told.
    let playheadFraction: Double?
    /// Backs the drag handle's .draggable() -- kept off the waveform
    /// body itself (see the grip overlay below) so it doesn't compete
    /// with the scrub DragGesture on the same view.
    let dragExport: ProcessedWavExport
    /// Fired continuously while dragging, and once more on release --
    /// both carry the shared-space fraction at the current/final
    /// location. `minimumDistance: 0` on the underlying gesture means a
    /// bare click fires both in the same instant, satisfying "click OR
    /// drag" with one gesture.
    let onScrubChanged: (Double) -> Void
    let onScrubEnded: (Double) -> Void

    private static let background = Color(red: 0.047, green: 0.078, blue: 0.063) // #0C1410, matches LCDReadoutView
    private static let dimGreen = Color(red: 0.25, green: 0.42, blue: 0.30)       // #3F6B4C
    private static let litGreen = Color(red: 0.56, green: 0.90, blue: 0.66)       // #8FE5A8

    /// The scale every trace, the start marker, and the playhead are ALL
    /// drawn against -- the longer of the two traces. Without this, an
    /// original and a time-stretched overlay of different lengths used
    /// to be drawn at different horizontal scales (each filling the full
    /// view width regardless of its own length), which made "where is
    /// the playhead, relative to either trace" ambiguous. A shorter
    /// trace now occupies a proportional PREFIX instead.
    private var _referenceCount: Int {
        max(samples.count, overlaySamples?.count ?? 0)
    }

    var body: some View {
        Canvas { context, size in
            if samples.isEmpty && overlaySamples == nil {
                return
            }
            let referenceCount = _referenceCount
            // Original underneath, dim -- drawn first so the processed
            // trace (if any) sits visually on top of it.
            _drawTrace(context: context, size: size, samples: samples, referenceCount: referenceCount, color: Self.dimGreen)
            if let overlay = overlaySamples {
                _drawTrace(context: context, size: size, samples: overlay, referenceCount: referenceCount, color: Self.litGreen)
            } else {
                // No processed render yet -- the original IS the
                // reference, so show it at full brightness rather than
                // dim (dim only makes sense as an "underneath" layer).
                _drawTrace(context: context, size: size, samples: samples, referenceCount: referenceCount, color: Self.litGreen)
            }
        }
        .frame(height: 90)
        .background(RoundedRectangle(cornerRadius: 6).fill(Self.background))
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .strokeBorder(Self.dimGreen.opacity(0.55), lineWidth: 1)
        )
        .overlay { _transportOverlay }
        .overlay(alignment: .topTrailing) {
            // The drag-out export handle -- deliberately a small,
            // visible grip rather than the whole waveform surface (which
            // used to carry .draggable() directly): a SwiftUI
            // DragGesture and an AppKit-backed .draggable() on the same
            // view don't compose predictably, and a bare tooltip was the
            // only clue the old whole-surface drag existed at all. This
            // both resolves the gesture conflict and makes drag-out
            // export discoverable.
            Image(systemName: "square.and.arrow.up")
                .font(.caption)
                .foregroundStyle(Self.dimGreen)
                .padding(6)
                .draggable(dragExport)
                .help("Drag out to export the processed audio as a .wav")
        }
    }

    /// Start-point dim wash + marker, and the playhead bar -- a separate
    /// overlay layer, not drawn inside the trace Canvas above. The
    /// Canvas recomputes min/max over the whole sample buffer on every
    /// invocation; putting the playhead there would mean redrawing that
    /// on every transport poll tick (20Hz) even though the traces
    /// themselves never change between polls. This overlay's own inputs
    /// (fractions) are cheap, so only it redraws.
    private var _transportOverlay: some View {
        GeometryReader { proxy in
            let width = proxy.size.width
            ZStack(alignment: .topLeading) {
                let startX = WaveformGeometry.x(forFraction: startFraction, width: width)
                if startFraction > 0 {
                    Rectangle()
                        .fill(Color.black.opacity(0.35))
                        .frame(width: startX)
                }
                Rectangle()
                    .fill(Self.litGreen)
                    .frame(width: 2)
                    .offset(x: startX - 1)

                if let playheadFraction {
                    let playX = WaveformGeometry.x(forFraction: playheadFraction, width: width)
                    Rectangle()
                        .fill(Color.white.opacity(0.85))
                        .frame(width: 1.5)
                        .offset(x: playX - 0.75)
                }
            }
            // GeometryReader's child does NOT auto-fill its proposed
            // size -- without this explicit frame, the ZStack sizes
            // itself to its widest child (a 1-2pt marker Rectangle), so
            // .contentShape/.gesture below would only be hit-testable
            // over a sliver near the marker, not the whole waveform.
            .frame(width: width, height: proxy.size.height, alignment: .topLeading)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        onScrubChanged(WaveformGeometry.fraction(forX: value.location.x, width: width))
                    }
                    .onEnded { value in
                        onScrubEnded(WaveformGeometry.fraction(forX: value.location.x, width: width))
                    }
            )
        }
    }

    /// Classic min/max-per-column waveform rendering: bucket the samples
    /// into one bucket per horizontal pixel and draw a vertical line
    /// spanning that bucket's peak-to-trough range. Cheap even for a
    /// multi-second, tens-of-thousands-of-samples buffer, since the work
    /// is O(sample count) regardless of view width. Column/bucket
    /// geometry lives in WaveformGeometry (AkaizerAudio) so it's unit-
    /// tested and shared with the fraction math above, rather than
    /// reimplemented by eye a second time here.
    private func _drawTrace(context: GraphicsContext, size: CGSize, samples: [Float], referenceCount: Int, color: Color) {
        guard !samples.isEmpty else { return }
        let columns = WaveformGeometry.columnCount(width: Double(size.width))
        let occupied = WaveformGeometry.occupiedColumns(count: samples.count, referenceCount: referenceCount, totalColumns: columns)
        guard occupied > 0 else { return }
        let midY = size.height / 2

        var path = Path()
        for column in 0..<occupied {
            let range = WaveformGeometry.bucketRange(column: column, count: samples.count, columns: occupied)
            guard !range.isEmpty else { continue }
            var minValue: Float = 0
            var maxValue: Float = 0
            for i in range {
                minValue = Swift.min(minValue, samples[i])
                maxValue = Swift.max(maxValue, samples[i])
            }
            let x = CGFloat(column)
            let yTop = midY - CGFloat(maxValue) * midY
            let yBottom = midY - CGFloat(minValue) * midY
            path.move(to: CGPoint(x: x, y: yTop))
            path.addLine(to: CGPoint(x: x, y: yBottom))
        }
        context.stroke(path, with: .color(color), lineWidth: 1)
    }
}
