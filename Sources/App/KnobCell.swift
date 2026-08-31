// KnobCell.swift
//
// One rack-panel knob: label above, RotaryKnobView, formatted value
// below -- doubling as the value readout the LCD's own summary rows
// still cover, but readable at a glance without hunting for the field in
// a wall of text.
//
// Extracted out of ContentView's old `_knobCell` *function* into its own
// View struct because double-click-to-type needs @State (which fields
// can't hold) to track whether the value readout is currently showing a
// TextField instead of Text.
//
// Double-click the knob opens the field; Return commits, Escape cancels,
// losing focus commits (so tabbing/clicking away doesn't silently
// discard a typed value the way Escape does). The field is seeded with a
// bare number, not the caller's display string, so parsing never has to
// know about any of the seven different suffix formats ("%", " st",
// " smp") -- it only ever has to read back a plain, possibly re-typed,
// number.

import AkaizerAudio
import SwiftUI

struct KnobCell: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let taper: KnobTaper
    let step: Double?
    let defaultValue: Double?
    let format: String
    var onEditingChanged: (Bool) -> Void = { _ in }

    @State private var editingText: String?
    @FocusState private var isFieldFocused: Bool

    var body: some View {
        VStack(spacing: 4) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)

            RotaryKnobView(
                value: $value, range: range, taper: taper, step: step, defaultValue: defaultValue,
                onEditingChanged: onEditingChanged,
                onRequestTextEntry: {
                    editingText = _trimmedNumber(value)
                    isFieldFocused = true
                }
            )

            if editingText != nil {
                TextField(
                    "", text: Binding(get: { editingText ?? "" }, set: { editingText = $0 })
                )
                .textFieldStyle(.plain)
                .multilineTextAlignment(.center)
                .font(.system(.caption, design: .monospaced))
                .frame(width: 64)
                .focused($isFieldFocused)
                .onSubmit { _commit() }
                .onKeyPress(.escape) { _cancel(); return .handled }
                .onChange(of: isFieldFocused) { _, focused in
                    if !focused { _commit() } // losing focus commits, not discards
                }
            } else {
                Text(String(format: format, value))
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.primary)
            }
        }
        .frame(width: 72)
    }

    /// A short, plain decimal -- e.g. "150", "0.25", "-12" -- with no
    /// unit suffix, so re-typing the same value round-trips cleanly.
    private func _trimmedNumber(_ v: Double) -> String {
        String(format: "%.4g", v)
    }

    private func _commit() {
        guard let text = editingText else { return }
        editingText = nil
        // Permissive parse: keep only digits/sign/decimal point, so a
        // pasted "150%" or "1000 smp" still parses, not just a bare
        // number.
        let filtered = text.filter { "0123456789.-".contains($0) }
        guard var parsed = Double(filtered) else { return }
        if let step, step > 0 {
            parsed = (parsed / step).rounded() * step
        }
        parsed = min(max(parsed, range.lowerBound), range.upperBound)
        guard parsed != value else { return }
        onEditingChanged(true)
        value = parsed
        onEditingChanged(false)
    }

    private func _cancel() {
        editingText = nil
    }
}
