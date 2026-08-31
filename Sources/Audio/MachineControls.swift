// MachineControls.swift
//
// v2 heritage-roster plan, stage 9: a single, testable source of truth
// for which knobs a machine shows, replacing ContentView's hand-written
// chain of `if`s. Lives in AkaizerAudio (not the App target) specifically
// so it's unit-testable -- ContentView.swift has zero tests today, and
// this is exactly the logic worth having them for: SP-1200 (no
// time-stretch, has a bandwidth control) must show Bandwidth + Cutoff +
// Transpose and NOTHING stretch-related; S950 (time-stretch AND
// bandwidth) must show both clusters together. Getting that combination
// right by hand, per machine, is exactly the kind of thing a descriptor
// table generated from profile capability flags gets right by
// construction instead.
//
// Deliberately data-only: this file returns a plain array ContentView
// iterates to build the knob row, rather than owning any SwiftUI. The
// app layer still holds each parameter as its own @State var (see
// ContentView.swift's own header note on why the v2 plan's SamplerModel
// view-model extraction was skipped) -- ContentView maps a ParamID to
// its @State var's Binding<Double> itself.

import AkaizerCore

/// Knob taper -- moved here from RotaryKnobView.swift (App target) so a
/// ControlDescriptor can specify it without the Audio target depending
/// on SwiftUI. RotaryKnobView.swift now imports AkaizerAudio for this
/// instead of declaring its own copy.
public enum KnobTaper {
    case linear
    case logarithmic
}

/// Identifies one of the ten (now eleven, with sampleRateHz) stretch
/// parameters -- what ContentView's per-ParamID binding switch keys on.
public enum ParamID: String, CaseIterable, Hashable {
    case transpose
    case bandwidth
    case cutoff
    case resonance
    case stretch
    case cycle
    case quality
    case width
}

/// Everything one knob needs, independent of any particular @State
/// storage. `lcdLabel` matches the real S3200XL time-stretch screen's
/// own lowercase convention (LCDReadoutView.swift), distinct from
/// `label`'s title-case rack-panel style.
public struct ControlDescriptor {
    public let id: ParamID
    public let label: String
    public let range: ClosedRange<Double>
    public let taper: KnobTaper
    public let step: Double?
    public let format: String
    public let lcdLabel: String

    public init(id: ParamID, label: String, range: ClosedRange<Double>, taper: KnobTaper, step: Double?, format: String, lcdLabel: String) {
        self.id = id
        self.label = label
        self.range = range
        self.taper = taper
        self.step = step
        self.format = format
        self.lcdLabel = lcdLabel
    }
}

public enum MachineControls {
    /// The knobs relevant for `machine` in `mode`, in rack-panel order.
    /// Driven entirely by AkzMachineProfile capability flags -- no
    /// `switch (machine)`, matching the DSP side's own discipline (see
    /// MachineProfile.cpp's header comment on why that matters).
    public static func controls(for machine: AkzMachine, mode: AkzStretchMode) -> [ControlDescriptor] {
        let profile = StretchProcessor.profile(for: machine)
        var result: [ControlDescriptor] = []

        // Transpose is a basic sampler feature every one of these
        // machines has (unlike time-stretch, which several lack), so
        // it's unconditional -- varispeed: pitch and duration move
        // together, matching the real hardware (Interpolator.h).
        result.append(ControlDescriptor(
            id: .transpose, label: "Transpose", range: -36...36, taper: .linear, step: 1,
            format: "%.0f st", lcdLabel: "transpose"
        ))

        // Bandwidth only means anything on a machine whose sample rate
        // is a control, not a fixed spec (S900/S950 today; scoped to
        // hasVariableSampleRate rather than every machine with a
        // min != max range, since a dual-FIXED-rate machine like S1000
        // wants a rate picker, not a knob -- not yet built). Shown in Hz
        // directly (the stored AkzStretchParams.sampleRateHz unit)
        // rather than converted back to the real "audio bandwidth"
        // figure real S900/S950 panels show (fs = bandwidth * 2.5) --
        // a deliberate simplification, not a citation that the real
        // control reads in Hz.
        if profile.hasVariableSampleRate != 0 {
            result.append(ControlDescriptor(
                id: .bandwidth, label: "Bandwidth",
                range: profile.minSampleRateHz...profile.maxSampleRateHz,
                taper: .logarithmic, step: 100,
                format: "%.0fHz", lcdLabel: "rate"
            ))
        }

        // Filter (build order stage 6) applies regardless of
        // time-stretch support -- every machine here has SOME VCF, so
        // cutoff is unconditional. Stays linear even though it feels
        // logarithmic in use: FilterModel already bends it to 20
        // Hz..Nyquist one layer down, so a log taper here would
        // double-bend the same curve.
        result.append(ControlDescriptor(
            id: .cutoff, label: "Cutoff", range: 0...1, taper: .linear, step: nil,
            format: "%.2f", lcdLabel: "cutoff"
        ))

        // Resonance only does anything on machines whose filter
        // actually has one (S2000/S3000/S3200 among the Akai six).
        if profile.filterHasResonance != 0 {
            result.append(ControlDescriptor(
                id: .resonance, label: "Resonance", range: 0...1, taper: .linear, step: nil,
                format: "%.2f", lcdLabel: "resonance"
            ))
        }

        if profile.supportsTimeStretch != 0 {
            // Stretch spans close to two orders of magnitude (25..2000%,
            // 25..999 on the S950) with the musically useful region
            // bunched near 100% -- logarithmic taper gives equal knob
            // rotation to equal *ratio* change.
            //
            // No `max(25.0, ...)` clamp here on purpose (v1/v2-pre-
            // stage-9 had one, defending against maxStretchPercent == 0
            // producing an invalid 25...0 range): the .stretch
            // descriptor is only ever appended when supportsTimeStretch
            // != 0, and a citation test
            // (MachineProfileTests/MachineControlsTests) asserts every
            // such machine's maxStretchPercent is a real value above 25
            // -- a bad profile should fail that test loudly, not clamp
            // silently into a hidden-but-technically-valid range.
            result.append(ControlDescriptor(
                id: .stretch, label: "Stretch",
                range: 25...profile.maxStretchPercent,
                taper: .logarithmic, step: 1,
                format: "%.0f%%", lcdLabel: "time factor"
            ))

            // Cycle length only means anything in CYCLIC; quality/width
            // only in INTELLIGENT -- shown accordingly rather than
            // all-visible-but-some-inert.
            let isIntelligent = profile.hasModeSwitch != 0 && mode == AkzStretchMode_Intelligent
            if isIntelligent {
                result.append(ControlDescriptor(
                    id: .quality, label: "Quality", range: 0...99, taper: .linear, step: 1,
                    format: "%.0f", lcdLabel: "qual"
                ))
                result.append(ControlDescriptor(
                    id: .width, label: "Width", range: 0...99, taper: .linear, step: 1,
                    format: "%.0f", lcdLabel: "width"
                ))
            } else {
                // Same span-of-two-orders-of-magnitude case as Stretch,
                // for the same reason: cycle length is felt/heard as a
                // ratio (an octave of grain length), not a linear
                // sample count.
                result.append(ControlDescriptor(
                    id: .cycle, label: "Cycle", range: 20...2000, taper: .logarithmic, step: 1,
                    format: "%.0f smp", lcdLabel: "cycle length"
                ))
            }
        }

        return result
    }
}
