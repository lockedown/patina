// LCDReadoutView.swift
//
// Build order stage 8: the one deliberately retro element in an
// otherwise native-Mac UI. Modelled on the real Akai S3200XL time-stretch
// screen documented in the project plan/research -- lowercase field
// labels, colons, mixed-case values (e.g. "stretch mode:  CYCLIC"), not
// a generic all-caps "green terminal" convention. The goal is a specific
// homage to that hardware's own screen, not "retro computing" in general.
//
// Deliberately does NOT follow the system light/dark appearance: a real
// LCD's phosphor colour doesn't change when the room lights do, and
// this is meant to read as a physical instrument panel, not a themed
// app surface. Everything else in the app uses standard adaptive system
// colours -- this is the one fixed-palette exception, on purpose.

import SwiftUI

/// One label/value pair on an LCD line, e.g. ("machine", "S950"). An
/// empty label renders as a bare value (used for a leading unlabelled
/// field like the sample name).
struct LCDField {
    let label: String
    let value: String

    init(_ label: String, _ value: String) {
        self.label = label
        self.value = value
    }
}

struct LCDReadoutView: View {
    /// Each inner array is the fields on one line, left to right.
    let rows: [[LCDField]]

    private static let background = Color(red: 0.047, green: 0.078, blue: 0.063)  // #0C1410
    private static let litGreen = Color(red: 0.56, green: 0.90, blue: 0.66)        // #8FE5A8 -- values
    private static let dimGreen = Color(red: 0.25, green: 0.42, blue: 0.30)        // #3F6B4C -- labels, border

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            ForEach(rows.indices, id: \.self) { rowIndex in
                HStack(spacing: 18) {
                    ForEach(rows[rowIndex].indices, id: \.self) { fieldIndex in
                        _fieldText(rows[rowIndex][fieldIndex])
                    }
                    Spacer(minLength: 0)
                }
            }
        }
        .font(.system(.callout, design: .monospaced).weight(.semibold))
        .tracking(1.1)
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 6).fill(Self.background))
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .strokeBorder(Self.dimGreen.opacity(0.55), lineWidth: 1)
        )
    }

    private func _fieldText(_ field: LCDField) -> Text {
        if field.label.isEmpty {
            return Text(field.value).foregroundColor(Self.litGreen)
        }
        return Text("\(field.label): ").foregroundColor(Self.dimGreen)
            + Text(field.value).foregroundColor(Self.litGreen)
    }
}
