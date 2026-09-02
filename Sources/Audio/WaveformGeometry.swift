// WaveformGeometry.swift
//
// Pure sample-index <-> x-position mapping for WaveformView.swift's
// Canvas, pulled out so it's unit-testable (Sources/App has no test
// target -- Package.swift's AkaizerAudioTests only covers Sources/Audio)
// and so the playhead/start-point overlay (2.1 feedback: "show playback
// bar over sample waveform... click and move start point") can share the
// exact same mapping the trace itself draws with, rather than
// approximating it independently and drifting out of alignment.
//
// Fixes a real bug on the way: WaveformView._drawTrace used to compute
// `samplesPerColumn = samples.count / columns` (integer division) and
// stop drawing once `column == columns`, which for any buffer between 1x
// and 2x the view's width in samples silently left the tail of the file
// undrawn (samplesPerColumn == 1, so the `column < columns` loop bound
// hits before `index` reaches samples.count). bucketRange below uses
// floating-point boundaries instead, so the last column's range always
// reaches exactly `count`.
//
// Also resolves a second, subtler issue: the original view had each
// trace (original vs. processed) compute its OWN samplesPerColumn, so a
// time-stretched overlay of a different length was drawn at a DIFFERENT
// horizontal scale than the original -- both traces filled the full
// view width regardless of their relative lengths, so a playhead
// couldn't align to both at once. occupiedColumns below maps every trace
// against one shared referenceCount (the longer of the two), so a
// shorter trace occupies a proportional PREFIX of the view instead of
// being stretched to fill it -- the correct depiction, and the one a
// single playhead fraction can point at unambiguously regardless of
// which buffer is currently playing.

import Foundation

public enum WaveformGeometry {
    /// One column per integer point of view width, same as the original
    /// implementation -- cheap (Canvas draws whole vertical lines, not
    /// per-sample), and fine-grained enough that a column boundary is
    /// never visually distinguishable from a true continuous plot.
    public static func columnCount(width: Double) -> Int {
        max(1, Int(width))
    }

    /// How many of `totalColumns` a trace of `count` samples should
    /// occupy when drawn to the same shared scale as a `referenceCount`-
    /// sample trace (typically the longer of original/processed) -- see
    /// the header comment above. `count == referenceCount` occupies all
    /// of them; a shorter trace occupies a proportional prefix.
    public static func occupiedColumns(count: Int, referenceCount: Int, totalColumns: Int) -> Int {
        guard referenceCount > 0, totalColumns > 0, count > 0 else { return 0 }
        let occupied = (Double(totalColumns) * Double(count) / Double(referenceCount)).rounded()
        return min(totalColumns, max(0, Int(occupied)))
    }

    /// The half-open sample-index range column `column` (of `columns`
    /// total, for a trace of `count` samples spread evenly across them)
    /// covers. Floating-point boundaries, not integer division -- see
    /// the header comment: this is what makes the LAST column's range
    /// always reach `count` exactly, rather than stopping short.
    public static func bucketRange(column: Int, count: Int, columns: Int) -> Range<Int> {
        guard count > 0, columns > 0, column >= 0, column < columns else { return 0..<0 }
        let start = Int((Double(column) * Double(count) / Double(columns)))
        let end = Int((Double(column + 1) * Double(count) / Double(columns)))
        let clampedStart = min(start, count)
        let clampedEnd = min(max(end, clampedStart), count)
        return clampedStart..<clampedEnd
    }

    /// x position (in points, 0...width) for a normalised [0, 1] fraction
    /// of playback through the shared reference length -- what the
    /// playhead bar and the start-point marker both use to place
    /// themselves against the SAME scale occupiedColumns/bucketRange
    /// draw the traces at.
    public static func x(forFraction fraction: Double, width: Double) -> Double {
        max(0, min(width, fraction * width))
    }

    /// Inverse of x(forFraction:width:), clamped to [0, 1] -- what a
    /// click/drag location on the waveform converts to before becoming a
    /// seek position or a new start point.
    public static func fraction(forX x: Double, width: Double) -> Double {
        guard width > 0 else { return 0 }
        return min(1, max(0, x / width))
    }
}
