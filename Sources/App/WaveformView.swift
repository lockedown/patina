// WaveformView.swift
//
// Build order stage 9: source and processed waveforms overlaid,
// phosphor-green -- matching LCDReadoutView's fixed retro palette (see
// that file's design rationale; this shares it deliberately, as one
// visual family). Original renders dim underneath; processed renders
// bright on top, so the shape of the edit is visible at a glance.
//
// Known limitation, flagged rather than silently omitted: this is
// display-only. The plan called the waveform "scrubable" -- clicking to
// seek playback mid-render -- which needs real transport plumbing
// (AVAudioPlayerNode doesn't support scrubbing a live playback position
// without stop-and-reschedule) that this stage didn't budget for.

import SwiftUI

struct WaveformView: View {
    /// Mono peak source, already deinterleaved to one channel (e.g.
    /// channel 0) -- the waveform's shape doesn't need every channel to
    /// be a useful visual reference.
    let samples: [Float]
    let overlaySamples: [Float]?

    private static let background = Color(red: 0.047, green: 0.078, blue: 0.063) // #0C1410, matches LCDReadoutView
    private static let dimGreen = Color(red: 0.25, green: 0.42, blue: 0.30)       // #3F6B4C
    private static let litGreen = Color(red: 0.56, green: 0.90, blue: 0.66)       // #8FE5A8

    var body: some View {
        Canvas { context, size in
            if samples.isEmpty && overlaySamples == nil {
                return
            }
            // Original underneath, dim -- drawn first so the processed
            // trace (if any) sits visually on top of it.
            _drawTrace(context: context, size: size, samples: samples, color: Self.dimGreen)
            if let overlay = overlaySamples {
                _drawTrace(context: context, size: size, samples: overlay, color: Self.litGreen)
            } else {
                // No processed render yet -- the original IS the
                // reference, so show it at full brightness rather than
                // dim (dim only makes sense as an "underneath" layer).
                _drawTrace(context: context, size: size, samples: samples, color: Self.litGreen)
            }
        }
        .frame(height: 90)
        .background(RoundedRectangle(cornerRadius: 6).fill(Self.background))
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .strokeBorder(Self.dimGreen.opacity(0.55), lineWidth: 1)
        )
    }

    /// Classic min/max-per-column waveform rendering: bucket the samples
    /// into one bucket per horizontal pixel and draw a vertical line
    /// spanning that bucket's peak-to-trough range. Cheap even for a
    /// multi-second, tens-of-thousands-of-samples buffer, since the work
    /// is O(sample count) regardless of view width.
    private func _drawTrace(context: GraphicsContext, size: CGSize, samples: [Float], color: Color) {
        guard !samples.isEmpty else { return }
        let columns = max(1, Int(size.width))
        let samplesPerColumn = max(1, samples.count / columns)
        let midY = size.height / 2

        var path = Path()
        var column = 0
        var index = 0
        while index < samples.count && column < columns {
            let end = min(index + samplesPerColumn, samples.count)
            var minValue: Float = 0
            var maxValue: Float = 0
            for i in index..<end {
                minValue = Swift.min(minValue, samples[i])
                maxValue = Swift.max(maxValue, samples[i])
            }
            let x = CGFloat(column)
            let yTop = midY - CGFloat(maxValue) * midY
            let yBottom = midY - CGFloat(minValue) * midY
            path.move(to: CGPoint(x: x, y: yTop))
            path.addLine(to: CGPoint(x: x, y: yBottom))

            index = end
            column += 1
        }
        context.stroke(path, with: .color(color), lineWidth: 1)
    }
}
