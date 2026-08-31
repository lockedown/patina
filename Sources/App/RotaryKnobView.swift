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
//
// User-feedback pass: double-click now opens inline text entry (the
// KnobCell wrapping this view owns the TextField -- see onRequestTextEntry
// below), so reset moved to Option-click, backed by a context-menu item
// so the moved affordance stays discoverable. Shift held during a drag
// switches to fine mode (see _dragGesture's header comment for the
// anchor math). onEditingChanged brackets every value-mutating
// interaction -- drag, reset, VoiceOver increment -- as one coalesced
// undo step for ContentView's undo stack.

import AkaizerAudio
import AppKit
import SwiftUI

struct RotaryKnobView: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let taper: KnobTaper
    /// Snap increment in real units (e.g. 1 semitone). nil = continuous.
    /// Bypassed while fine-dragging (Shift held) -- see _dragGesture.
    let step: Double?
    /// Double-click/tap resets to this value -- the closest software
    /// analogue to a hardware pot's own printed centre mark. Reset is
    /// reached via Option-click now that double-click opens text entry.
    let defaultValue: Double?
    /// Fired true immediately before the first mutation of `value` in a
    /// drag/reset/accessibility-increment, and false when it ends --
    /// mirrors SwiftUI's own Slider onEditingChanged. ContentView uses
    /// this to bracket one undo step per interaction, however many
    /// individual `value` writes happen inside it.
    var onEditingChanged: (Bool) -> Void = { _ in }
    /// Fired on double-click. The knob has no room (or, for a hardware-
    /// style dial, the right shape) for an inline text field, so the
    /// enclosing KnobCell owns the TextField and this is just the
    /// request to show it.
    var onRequestTextEntry: () -> Void = {}

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
    /// Shift divides sensitivity by this -- a full sweep becomes 750pt
    /// instead of 150pt. Chosen against the two knobs with the widest
    /// feel: Cutoff's continuous 0...1 becomes ~0.0013/pt (fine enough to
    /// dial in a precise cutoff by ear) and Cycle's ~100:1 log range
    /// becomes ~0.6%/pt (still reachable in a reasonable drag, unlike a
    /// much larger divisor would leave it).
    private let fineDivisor: Double = 5

    @State private var dragStartValue: Double?
    /// Anchor pair for the drag, re-seated every time Shift is pressed or
    /// released mid-drag. Scaling `translation.height` by the fine
    /// divisor directly (instead of anchoring) would retroactively
    /// rescale the whole drag and make the value jump the instant the
    /// modifier changes; re-anchoring to (current normalized value,
    /// current raw translation) at the moment of the change makes every
    /// subsequent delta measured fresh from there, so the transition is
    /// seamless in both directions, any number of times per drag.
    @State private var anchorNormalized: Double?
    @State private var anchorHeight: CGFloat?
    @State private var wasFineDragging = false

    init(
        value: Binding<Double>, range: ClosedRange<Double>,
        taper: KnobTaper = .linear, step: Double? = nil, defaultValue: Double? = nil,
        onEditingChanged: @escaping (Bool) -> Void = { _ in },
        onRequestTextEntry: @escaping () -> Void = {}
    ) {
        _value = value
        self.range = range
        self.taper = taper
        self.step = step
        self.defaultValue = defaultValue
        self.onEditingChanged = onEditingChanged
        self.onRequestTextEntry = onRequestTextEntry
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
        // Order matters: SwiftUI resolves multiple .onTapGesture modifiers
        // by count, so declaring count:2 first and count:1 second lets a
        // double-click still register as a double-click rather than two
        // single-clicks. count:1 checks for Option itself (.onTapGesture
        // carries no modifier info on macOS) and no-ops otherwise, so a
        // bare single click still does nothing, as before.
        .onTapGesture(count: 2) { onRequestTextEntry() }
        .onTapGesture(count: 1) {
            if NSEvent.modifierFlags.contains(.option) { _resetToDefault() }
        }
        .contextMenu {
            Button("Reset to Default") { _resetToDefault() }
                .disabled(defaultValue == nil)
        }
        .help("Drag to adjust · hold Shift to fine-tune · double-click to type a value · ⌥-click to reset")
        .accessibilityElement()
        .accessibilityValue(String(format: "%.2f", value))
        .accessibilityAdjustableAction { direction in
            let delta = step ?? (range.upperBound - range.lowerBound) / 100
            onEditingChanged(true)
            switch direction {
            case .increment: value = min(range.upperBound, value + delta)
            case .decrement: value = max(range.lowerBound, value - delta)
            @unknown default: break
            }
            onEditingChanged(false)
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
                let shift = NSEvent.modifierFlags.contains(.shift)

                if dragStartValue == nil {
                    dragStartValue = value
                    onEditingChanged(true)
                    anchorNormalized = _toNormalized(value)
                    anchorHeight = drag.translation.height
                    wasFineDragging = shift
                } else if shift != wasFineDragging {
                    // Modifier changed mid-drag -- re-anchor to the
                    // current value and current raw translation so the
                    // next delta is measured fresh from here, not
                    // retroactively rescaled from drag start.
                    anchorNormalized = _toNormalized(value)
                    anchorHeight = drag.translation.height
                    wasFineDragging = shift
                }

                guard let anchorT = anchorNormalized, let anchorH = anchorHeight else { return }
                let sensitivity = dragTravel * (shift ? fineDivisor : 1)
                let deltaT = Double(-(drag.translation.height - anchorH)) / sensitivity
                var newValue = _fromNormalized(anchorT + deltaT)
                // Fine mode bypasses step snapping -- the whole point of
                // Shift is sub-step precision (e.g. sub-semitone
                // transpose, which the underlying param is a float and
                // genuinely supports), so snapping to the same increment
                // while fine-dragging would make the modifier a no-op on
                // every stepped knob.
                if !shift, let step, step > 0 {
                    newValue = (newValue / step).rounded() * step
                }
                value = min(max(newValue, range.lowerBound), range.upperBound)
            }
            .onEnded { _ in
                dragStartValue = nil
                anchorNormalized = nil
                anchorHeight = nil
                wasFineDragging = false
                onEditingChanged(false)
            }
    }

    private func _resetToDefault() {
        guard let defaultValue else { return }
        onEditingChanged(true)
        value = min(max(defaultValue, range.lowerBound), range.upperBound)
        onEditingChanged(false)
    }
}
