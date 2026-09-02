// WaveformGeometryTests.swift
//
// See WaveformGeometry.swift's header for the two bugs this pins:
// (1) the old integer-division samplesPerColumn left the tail of a
// buffer between 1x and 2x the view width undrawn, and (2) two traces
// of different lengths were drawn at different horizontal scales,
// which a single playhead fraction couldn't align to.

import XCTest

@testable import AkaizerAudio

final class WaveformGeometryTests: XCTestCase {
    func testColumnCountIsAtLeastOneEvenForZeroWidth() {
        XCTAssertEqual(WaveformGeometry.columnCount(width: 0), 1)
        XCTAssertEqual(WaveformGeometry.columnCount(width: 0.4), 1)
        XCTAssertEqual(WaveformGeometry.columnCount(width: 200.9), 200)
    }

    /// The regression test for the truncation bug: the last column's
    /// bucket must reach exactly `count`, for every combination of a
    /// sample count between 1x and 2x the column count (the exact range
    /// where `count / columns` integer division used to equal 1 and the
    /// old `column < columns` loop bound cut the trace short).
    func testLastColumnAlwaysReachesTheFullSampleCount() {
        let columns = 300
        for count in stride(from: columns, through: columns * 2, by: 37) {
            let range = WaveformGeometry.bucketRange(column: columns - 1, count: count, columns: columns)
            XCTAssertEqual(range.upperBound, count, "count=\(count) columns=\(columns)")
        }
    }

    func testBucketRangesAreContiguousAndCoverEverySample() {
        let count = 4173
        let columns = 300
        var previousEnd = 0
        for column in 0..<columns {
            let range = WaveformGeometry.bucketRange(column: column, count: count, columns: columns)
            XCTAssertEqual(range.lowerBound, previousEnd, "gap or overlap before column \(column)")
            previousEnd = range.upperBound
        }
        XCTAssertEqual(previousEnd, count)
    }

    func testBucketRangeOutOfBoundsColumnIsEmpty() {
        XCTAssertEqual(WaveformGeometry.bucketRange(column: -1, count: 100, columns: 10), 0..<0)
        XCTAssertEqual(WaveformGeometry.bucketRange(column: 10, count: 100, columns: 10), 0..<0)
    }

    func testBucketRangeWithZeroCountOrColumnsIsEmpty() {
        XCTAssertEqual(WaveformGeometry.bucketRange(column: 0, count: 0, columns: 10), 0..<0)
        XCTAssertEqual(WaveformGeometry.bucketRange(column: 0, count: 10, columns: 0), 0..<0)
    }

    func testOccupiedColumnsAtFullReferenceLengthFillsEveryColumn() {
        XCTAssertEqual(WaveformGeometry.occupiedColumns(count: 1000, referenceCount: 1000, totalColumns: 300), 300)
    }

    /// A trace shorter than the reference occupies a proportional
    /// PREFIX, not the full width -- the fix for the two-different-
    /// scales bug.
    func testShorterTraceOccupiesAProportionalPrefix() {
        let occupied = WaveformGeometry.occupiedColumns(count: 500, referenceCount: 1000, totalColumns: 300)
        XCTAssertEqual(occupied, 150)
    }

    func testOccupiedColumnsWithNoSamplesIsZero() {
        XCTAssertEqual(WaveformGeometry.occupiedColumns(count: 0, referenceCount: 1000, totalColumns: 300), 0)
    }

    func testXAndFractionRoundTrip() {
        for fraction in stride(from: 0.0, through: 1.0, by: 0.1) {
            let x = WaveformGeometry.x(forFraction: fraction, width: 400)
            XCTAssertEqual(WaveformGeometry.fraction(forX: x, width: 400), fraction, accuracy: 1e-9)
        }
    }

    func testFractionClampsOutOfRangeX() {
        XCTAssertEqual(WaveformGeometry.fraction(forX: -50, width: 400), 0)
        XCTAssertEqual(WaveformGeometry.fraction(forX: 500, width: 400), 1)
    }

    func testFractionWithZeroWidthIsZero() {
        XCTAssertEqual(WaveformGeometry.fraction(forX: 10, width: 0), 0)
    }

    func testXClampsToWidthForOutOfRangeFraction() {
        XCTAssertEqual(WaveformGeometry.x(forFraction: 1.5, width: 400), 400)
        XCTAssertEqual(WaveformGeometry.x(forFraction: -0.5, width: 400), 0)
    }
}
