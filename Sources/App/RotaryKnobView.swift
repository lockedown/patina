// RotaryKnobView.swift
//
// Rotary knob control modelled on the pots on a real Akai S-series rack
// unit: a dark knurled body, a single indicator line, and a lit value
// arc borrowed from LCDReadoutView's own phosphor-green palette so the
// two "physical panel" elements read as one instrument rather than two
// unrelated skins. Everything else in the app stays native-Mac controls
// (see LCDReadoutView.swift's header) -- this is a second, deliberate
// exception, scoped to the stretch/filter parameters that behave like
// hardware knobs rather than software sliders.
//
// Taper matters here as much as the shape: a knob's travel is wasted if
// it doesn't match the parameter's real dynamic range. Stretch
// (25..2000%) and Cycle length (20..2000 smp) both span close to two
// orders of magnitude with the musically useful region bunched near the
// low end, so those two get a logarithmic taper -- equal rotation gives
// equal *ratio* change, which is how time-stretch and grain length are
// actually heard. Transpose, Cutoff, Resonance, Quality and Width are
// all linear -- Cutoff in particular is already log-mapped one layer
// down, to 20 Hz..Nyquist by FilterModel, so a log taper here would
// double-bend the same curve rather than help it.

import SwiftUI

enum KnobTaper {
    case linear
    case logarithmic
}

struct RotaryKnobView: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let taper: KnobTaper
    /// Snap increment in real units (e.g. 1 semitone). nil = continuous.
    let step: Double?
    /// Double-click/tap resets to this value -- the closest software
    /// analogue to a hardware pot's own printed centre mark.
    let defaultValue: Double?

    // Same two greens as LCDReadoutView, on purpose -- see that file's
    // header for why the LCD ignores system appearance; this control
    // does too, for the same "physical panel" reason.
    private static let track = Color(red: 0.25, green: 0.42, blue: 0.30)  // dimGreen
    private static let lit = Color(red: 0.56, green: 0.90, blue: 0.66)    // litGreen

    private let diameter: CGFloat = 44
    private let minAngle: Double = -135
    private let maxAngle: Double = 135
    /// Vertical drag distance, in points, for the full range sweep.
    /// A literal circular drag fights trackpads and mice -- a single
    /// vertical pull is how most software knobs and every DAW plugin
    /// knob actually work.
    private let dragTravel: Double = 150

    @State private var dragStartValue: Double?

    init(
        value: Binding<Double>, range: ClosedRange<Double>,
        taper: KnobTaper = .linear, step: Double? = nil, defaultValue: Double? = nil
    ) {
        _value = value
        self.range = range
        self.taper = taper
        self.step = step
        self.defaultValue = defaultValue
    }

    var body: some View {
        ZStack {
            Circle()
                .trim(from: 0, to: 0.75)
                .stroke(Self.track.opacity(0.5), style: StrokeStyle(lineWidth: 3, lineCap: .round))
                .rotationEffect(.degrees(minAngle - 90))
                .frame(width: diameter + 14, height: diameter + 14)

            Circle()
                .trim(from: 0, to: 0.75 * _normalized)
                .stroke(Self.lit, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                .rotationEffect(.degrees(minAngle - 90))
                .frame(width: diameter + 14, height: diameter + 14)

            Circle()
                .fill(
                    RadialGradient(
                        colors: [Color(white: 0.34), Color(white: 0.11)],
                        center: UnitPoint(x: 0.35, y: 0.3),
                        startRadius: 1,
                        endRadius: diameter * 0.7
                    )
                )
                .frame(width: diameter, height: diameter)
                .overlay(Circle().strokeBorder(Color.black.opacity(0.6), lineWidth: 1))

            RoundedRectangle(cornerRadius: 1.2)
                .fill(Self.lit)
                .frame(width: 2.5, height: diameter * 0.36)
                .offset(y: -diameter * 0.2)
                .rotationEffect(.degrees(minAngle + _normalized * (maxAngle - minAngle)))
        }
        .contentShape(Circle().inset(by: -8))
        .gesture(_dragGesture)
        .onTapGesture(count: 2) { _resetToDefault() }
        .accessibilityElement()
        .accessibilityValue(String(format: "%.2f", value))
        .accessibilityAdjustableAction { direction in
            let delta = step ?? (range.upperBound - range.lowerBound) / 100
            switch direction {
            case .increment: value = min(range.upperBound, value + delta)
            case .decrement: value = max(range.lowerBound, value - delta)
            @unknown default: break
            }
        }
    }

    private var _normalized: Double { _toNormalized(value) }

    private func _toNormalized(_ v: Double) -> Double {
        let clamped = min(max(v, range.lowerBound), range.upperBound)
        switch taper {
        case .linear:
            guard range.upperBound > range.lowerBound else { return 0 }
            return (clamped - range.lowerBound) / (range.upperBound - range.lowerBound)
        case .logarithmic:
            // Ranges here are always positive (percentages, sample
            // counts), but clamp away from 0 defensively -- log(0) is
            // undefined and a machine profile could in principle hand
            // back a degenerate 0-width range (see the S900 "25...0"
            // guard already in ContentView).
            let lo = max(range.lowerBound, 0.0001)
            let hi = max(range.upperBound, lo * 1.0001)
            let v2 = max(clamped, lo)
            return log(v2 / lo) / log(hi / lo)
        }
    }

    private func _fromNormalized(_ t: Double) -> Double {
        let clampedT = min(max(t, 0), 1)
        switch taper {
        case .linear:
            return range.lowerBound + clampedT * (range.upperBound - range.lowerBound)
        case .logarithmic:
            let lo = max(range.lowerBound, 0.0001)
            let hi = max(range.upperBound, lo * 1.0001)
            return lo * pow(hi / lo, clampedT)
        }
    }

    private var _dragGesture: some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { drag in
                if dragStartValue == nil { dragStartValue = value }
                guard let start = dragStartValue else { return }
                let deltaT = Double(-drag.translation.height) / dragTravel
                let newT = _toNormalized(start) + deltaT
                var newValue = _fromNormalized(newT)
                if let step, step > 0 {
                    newValue = (newValue / step).rounded() * step
                }
                value = min(max(newValue, range.lowerBound), range.upperBound)
            }
            .onEnded { _ in dragStartValue = nil }
    }

    private func _resetToDefault() {
        guard let defaultValue else { return }
        value = min(max(defaultValue, range.lowerBound), range.upperBound)
    }
}
