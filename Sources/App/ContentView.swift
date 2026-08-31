// ContentView.swift
//
// Build order stage 3/4 checkpoint: the stretch engine wired to real
// audio, both offline (whole-buffer) rendering and live audition, plus
// playback so the documented artifacts (metallic ringing at short cycle
// lengths, tremolo at long ones -- plan "2.4") can actually be judged by
// ear, not just by the autocorrelation tests in
// Tests/CoreTests/ArtifactTests.cpp.
//
// Two parallel, independent paths on purpose, not one path trying to
// serve both jobs:
//   - Process/Play/Save: offline, via StretchProcessor -- unchanged from
//     stage 3, still the right tool for "render the whole thing and
//     commit it to a file."
//   - Live Audition toggle: continuous, via LiveAuditionController --
//     starts an AVAudioSourceNode loop that keeps playing whatever the
//     background thread most recently rendered, and every slider/picker
//     change pushes new params to it live. See LiveAuditionController.swift.
//
// Build order stage 8: sidebar (Recent files -- a jump list, not a
// batch queue, matching the "single-file focus" workflow decision) plus
// the LCDReadoutView retro accent, otherwise standard native-Mac
// controls/layout. See LCDReadoutView.swift for the design rationale.
//
// Build order stage 9, the last one: named presets (PresetStore.swift),
// a waveform display (WaveformView.swift -- display only, not scrubable;
// see that file's header for why), and loudness-matched A/B between
// Play Original and Play Processed (PCMConversion's rms/matchedGain --
// "compares processed against dry original at matched loudness," the
// plan's own words).

import AkaizerAudio
import AkaizerCore
import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @State private var loadedSample: LoadedSample?
    @State private var statusMessage: String = "No file loaded."
    @State private var lastVerifyResult: String?

    @State private var selectedMachine: AkzMachine = AkzMachine_S950
    @State private var selectedEngine: AkzEngine = AkzEngine_Classic
    @State private var selectedMode: AkzStretchMode = AkzStretchMode_Cyclic
    @State private var stretchPercent: Double = 100
    @State private var cycleLength: Double = 1000
    @State private var quality: Double = 10
    @State private var width: Double = 10
    @State private var transposeSemitones: Double = 0
    @State private var filterCutoff: Double = 1.0
    @State private var filterResonance: Double = 0.0
    @State private var processedChannels: [[Float]]?

    @State private var isLiveAuditionOn = false
    @State private var liveController: LiveAuditionController?

    /// True while a Finder drag is hovering the window -- drives the
    /// drop-target highlight overlay only.
    @State private var isDropTargeted = false

    /// Undo/redo covers the ten parameters only (see ParamSnapshot) -- not
    /// file loads, not the processed render. A knob drag collapses into
    /// one step via _beginParamEdit/_endParamEdit, bracketed by
    /// RotaryKnobView's onEditingChanged; discrete controls (Engine,
    /// Mode, Machine) push a step directly from their binding setter
    /// instead, since they have no drag to bracket.
    @State private var undoStack: [ParamSnapshot] = []
    @State private var redoStack: [ParamSnapshot] = []
    /// The snapshot taken when the currently-open drag/edit began, if
    /// any. Closed (and, if the value actually changed, pushed to
    /// undoStack) by _endParamEdit.
    @State private var pendingEditSnapshot: ParamSnapshot?
    private let maxUndoDepth = 64

    /// Params the current processedChannels render was made with -- lets
    /// the UI (and the drag-out export) tell a stale render from a fresh
    /// one without disabling anything, since a stale render is still
    /// valid audio.
    @State private var renderedSnapshot: ParamSnapshot?

    /// "Recomputing" busy light for the Preview button (README's "no
    /// visual cue during a slow re-render" gap). Polled rather than
    /// pushed -- LiveAuditionController's readiness lives on a
    /// std::atomic<bool> written by a C++ worker thread, so there's
    /// nothing to subscribe to; polling a render-thread-safe atomic from
    /// the main thread is the same discipline the app already uses for
    /// isReady/hasPendingCommit. Hysteresis (150ms delay before showing,
    /// 250ms minimum once shown) keeps it from strobing on every knob
    /// tick during a run of fast, filter-only re-renders.
    @State private var isRecomputingVisible = false
    @State private var _recomputingBusySince: Date?
    @State private var _recomputingVisibleUntil: Date?
    private let _recomputingPollTimer = Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()

    /// True while AudioPlaybackController is playing (Play Original or
    /// Play Processed) -- drives the Stop button's enabled state and
    /// nothing else, so it must track actual play/finish/stop, not just
    /// "a play button was pressed."
    @State private var isPlayingOffline = false

    /// Most-recently-opened files, newest first, capped -- a quick-switch
    /// jump list for the sidebar, not the batch queue the plan
    /// explicitly ruled out. Session-only for now; stage 9 (presets) may
    /// give this real persistence.
    @State private var recentFiles: [URL] = []
    private let maxRecentFiles = 8

    /// Named parameter sets -- "Jungle S950," "Dusty S1000," the plan's
    /// own examples. Persisted via PresetStore, loaded once on launch.
    @State private var presets: [AkaizerPreset] = []

    /// Mono (channel 0) traces for WaveformView. Decoded once per load/
    /// process rather than in the view body, so scrolling/resizing the
    /// window doesn't re-decode a whole file's worth of PCM every frame.
    @State private var originalWaveformSamples: [Float] = []
    @State private var processedWaveformSamples: [Float]?

    private let audioFileService = AudioFileService()
    private let playback = AudioPlaybackController()
    private let presetStore = PresetStore()

    private var machineProfile: AkzMachineProfile {
        StretchProcessor.profile(for: selectedMachine)
    }

    /// The current machine's documented defaults -- used as each knob's
    /// double-click reset target, so "reset" always means "this
    /// machine's real default," not a hardcoded number that drifts once
    /// another machine is selected.
    private var _defaultParams: AkzStretchParams {
        StretchProcessor.defaultParams(machine: selectedMachine)
    }

    private var _stretchIsSupported: Bool {
        machineProfile.supportsTimeStretch != 0
    }

    private var _filterHasResonance: Bool {
        machineProfile.filterHasResonance != 0
    }

    private var _hasModeSwitch: Bool {
        machineProfile.hasModeSwitch != 0
    }

    private var _isIntelligentMode: Bool {
        _hasModeSwitch && selectedMode == AkzStretchMode_Intelligent
    }

    /// LCD readout content -- see LCDReadoutView.swift. Field names
    /// deliberately echo the real S3200XL time-stretch screen's own
    /// lowercase "label: value" convention from the project research,
    /// not an invented all-caps "terminal" style.
    private var _lcdRows: [[LCDField]] {
        var rows: [[LCDField]] = []

        if let sample = loadedSample {
            rows.append([
                LCDField("sample", sample.url.lastPathComponent),
                LCDField("", "\(Int(sample.sampleRateHz))hz"),
                LCDField("", "\(sample.bitsPerSample)-bit"),
            ])
        } else {
            rows.append([LCDField("sample", "(none loaded)")])
        }

        rows.append([
            LCDField("machine", machineProfile.displayName),
            LCDField("engine", selectedEngine == AkzEngine_Classic ? "CLASSIC" : "REVISED"),
        ])

        if _stretchIsSupported {
            if _isIntelligentMode {
                rows.append([
                    LCDField("time factor", "\(Int(stretchPercent))%"),
                    LCDField("qual", "\(Int(quality))"),
                    LCDField("width", "\(Int(width))"),
                ])
            } else {
                rows.append([
                    LCDField("time factor", "\(Int(stretchPercent))%"),
                    LCDField("cycle length", "\(Int(cycleLength))"),
                ])
            }
        } else {
            rows.append([LCDField("time factor", "n/a -- no time-stretch")])
        }

        var filterFields = [
            LCDField("transpose", "\(Int(transposeSemitones))st"),
            LCDField("cutoff", String(format: "%.2f", filterCutoff)),
        ]
        if _filterHasResonance {
            filterFields.append(LCDField("resonance", String(format: "%.2f", filterResonance)))
        }
        rows.append(filterFields)

        return rows
    }

    var body: some View {
        HStack(spacing: 0) {
            _sidebar
            Divider()
            _mainContent
        }
        .frame(minWidth: 720, minHeight: 600)
        // Attached to the whole window's root, not to any child --
        // drag-and-drop is NSDraggingDestination, a different mechanism
        // from gesture recognition, so nested ScrollViews and the knobs'
        // own DragGestures never consume it. One modifier here genuinely
        // covers the sidebar, the knobs, and the horizontal knob
        // scroller, matching the feedback's "drop anywhere in the
        // window."
        // Declared types match openFile()'s NSOpenPanel filter ([.wav,
        // .aiff]) rather than the generic .fileURL -- Finder's own drag
        // eligibility check (the "no" cursor) then rejects a .txt or
        // .mp3 before this view is even asked, no extension-sniffing
        // needed here. .aiff covers both .aiff and .aif by UTType
        // conformance. onDrop with a URL-conforming NSItemProvider
        // (rather than .dropDestination(for: URL.self), which has a
        // documented history of failing to decode Finder's own
        // public.file-url payload) is the reliable path -- verified
        // against a real Finder drag, not just the simulator/preview.
        .onDrop(of: [.wav, .aiff], isTargeted: $isDropTargeted, perform: _handleDrop)
        .overlay {
            if isDropTargeted {
                RoundedRectangle(cornerRadius: 8)
                    .strokeBorder(Color.accentColor, lineWidth: 3)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Color.accentColor.opacity(0.08)))
                    .overlay(
                        Text("Drop a WAV or AIFF to load it")
                            .font(.headline)
                            .foregroundStyle(Color.accentColor)
                    )
                    .padding(8)
                    .allowsHitTesting(false)
            }
        }
        .onAppear {
            presets = presetStore.load()
            playback.onFinished = { isPlayingOffline = false }
            _autoloadIfRequested()
            _sweepDragExportTempFiles()
        }
        // Bridges the Edit menu's Undo/Redo (AkaizerSApp.swift's
        // ParamEditMenuCommands, EditCommands.swift) to this struct's own
        // @State-backed undo stack -- see EditCommands.swift's header for
        // why a Scene-level menu can't reach a plain @State var directly.
        .focusedSceneValue(
            \.paramEdit,
            ParamEditCommands(
                canUndo: !undoStack.isEmpty, canRedo: !redoStack.isEmpty,
                undo: _undo, redo: _redo
            )
        )
    }

    // -- layout --------------------------------------------------------------

    private var _sidebar: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("RECENT")
                .font(.caption.weight(.semibold))
                .tracking(1.0)
                .foregroundStyle(.secondary)
                .padding(.top, 16)
                .padding(.horizontal, 14)
                .padding(.bottom, 6)

            if recentFiles.isEmpty {
                Text("Open a file to get started.")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .padding(.horizontal, 14)
            } else {
                ForEach(recentFiles, id: \.self) { url in
                    _recentFileRow(url)
                }
            }

            Text("PRESETS")
                .font(.caption.weight(.semibold))
                .tracking(1.0)
                .foregroundStyle(.secondary)
                .padding(.top, 18)
                .padding(.horizontal, 14)
                .padding(.bottom, 6)

            if presets.isEmpty {
                Text("No presets saved yet.")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .padding(.horizontal, 14)
            } else {
                ForEach(presets) { preset in
                    _presetRow(preset)
                }
            }

            Spacer()
        }
        .frame(width: 190)
        .frame(maxHeight: .infinity)
        .background(Color(nsColor: .controlBackgroundColor))
    }

    private func _presetRow(_ preset: AkaizerPreset) -> some View {
        HStack(spacing: 4) {
            Button(action: { _applyPreset(preset) }) {
                Text(preset.name)
                    .font(.callout)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .buttonStyle(.plain)

            Button(action: { _deletePreset(preset) }) {
                Image(systemName: "xmark.circle.fill")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            .buttonStyle(.plain)
            .help("Delete preset")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 5)
    }

    private func _recentFileRow(_ url: URL) -> some View {
        let isCurrent = loadedSample?.url == url
        return Button(action: { _load(url: url) }) {
            HStack(spacing: 7) {
                Circle()
                    .fill(isCurrent ? Color.accentColor : .clear)
                    .frame(width: 6, height: 6)
                Text(url.lastPathComponent)
                    .font(.callout)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .foregroundStyle(isCurrent ? Color.primary : Color.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 14)
            .padding(.vertical, 5)
        }
        .buttonStyle(.plain)
        .background(isCurrent ? Color.accentColor.opacity(0.12) : Color.clear)
    }

    private var _mainContent: some View {
        // A ScrollView backstop, not just a fixed-size VStack: the
        // number of visible knobs/rows changes with machine and mode
        // (Intelligent adds Quality+Width, Resonance appears/disappears,
        // the waveform only shows once a file's loaded), so the content
        // height isn't fixed the way the window's minHeight is. Without
        // this, a tall combination pushes Process/Play/Stop/Save below
        // the window's bottom edge instead of just scrolling to them.
        ScrollView(.vertical) {
            VStack(alignment: .leading, spacing: 16) {
            HStack {
                Button("Open WAV/AIFF…", action: openFile)
                Button("Save Unchanged Copy…", action: saveCopy)
                    .disabled(loadedSample == nil)
                Button("Verify Round Trip", action: verifyRoundTrip)
                    .disabled(loadedSample == nil)
                Divider()
                Button { _undo() } label: { Image(systemName: "arrow.uturn.backward") }
                    .disabled(undoStack.isEmpty)
                    .help("Undo the last parameter change (⌘Z)")
                Button { _redo() } label: { Image(systemName: "arrow.uturn.forward") }
                    .disabled(redoStack.isEmpty)
                    .help("Redo (⇧⌘Z)")
            }

            LCDReadoutView(rows: _lcdRows)

            if let sample = loadedSample {
                WaveformView(samples: originalWaveformSamples, overlaySamples: processedWaveformSamples)
                    .draggable(_dragExport(for: sample))
                    .help("Drag out to export the processed audio as a .wav")
            }

            if let sample = loadedSample {
                HStack(spacing: 16) {
                    Text("\(sample.channelCount) ch")
                    Text("\(sample.frameCount) frames")
                    Text(String(format: "%.3fs", sample.durationSeconds))
                }
                .font(.caption)
                .foregroundStyle(.secondary)
            }

            if let verifyResult = lastVerifyResult {
                Text(verifyResult)
                    .font(.callout)
                    .foregroundStyle(verifyResult.hasPrefix("✓") ? .green : .red)
            }

            Divider()

            GroupBox("Time stretch") {
                VStack(alignment: .leading, spacing: 10) {
                    Picker("Machine", selection: Binding(get: { selectedMachine }, set: _selectMachine)) {
                        ForEach(StretchProcessor.allMachines, id: \.rawValue) { machine in
                            Text(StretchProcessor.profile(for: machine).displayName).tag(machine)
                        }
                    }
                    .onChange(of: selectedEngine) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: selectedMode) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: stretchPercent) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: cycleLength) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: quality) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: width) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: transposeSemitones) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: filterCutoff) { _, _ in _pushLiveParamsIfNeeded() }
                    .onChange(of: filterResonance) { _, _ in _pushLiveParamsIfNeeded() }

                    Picker("Engine", selection: _undoableBinding(get: { selectedEngine }, set: { selectedEngine = $0 })) {
                        Text("Classic").tag(AkzEngine_Classic)
                        Text("Revised").tag(AkzEngine_Revised)
                    }
                    .pickerStyle(.segmented)

                    if _stretchIsSupported && _hasModeSwitch {
                        Picker("Mode", selection: _undoableBinding(get: { selectedMode }, set: { selectedMode = $0 })) {
                            Text("Cyclic").tag(AkzStretchMode_Cyclic)
                            Text("Intelligent").tag(AkzStretchMode_Intelligent)
                        }
                        .pickerStyle(.segmented)
                    }

                    // One horizontal row for every knob currently
                    // relevant -- a real Akai rack panel is one strip of
                    // pots, not a stack of separate control clusters, and
                    // stacking rows here was pushing Process/Play/Stop
                    // below the bottom of the window on some machine/mode
                    // combinations. ScrollView on the whole pane (below)
                    // is the backstop; a single flat row is the fix that
                    // actually keeps the common case short.
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(alignment: .top, spacing: 20) {
                            // Transpose is a basic sampler feature every
                            // one of these machines has (unlike
                            // time-stretch, which the S900 lacks -- plan
                            // section 3.1), so it's shown unconditionally
                            // rather than gated on _stretchIsSupported.
                            // Varispeed: pitch and duration move
                            // together, matching the real hardware -- see
                            // Interpolator.h.
                            _knobCell(
                                "Transpose", value: $transposeSemitones,
                                range: -36...36, taper: .linear, step: 1,
                                defaultValue: Double(_defaultParams.transposeSemitones),
                                format: "%.0f st"
                            )

                            // Filter (build order stage 6) applies
                            // regardless of whether the machine supports
                            // time-stretch -- every one of these machines
                            // has SOME VCF (plan section 3.1), so cutoff
                            // is shown unconditionally. Cutoff's own 0..1
                            // knob stays linear even though it feels
                            // logarithmic in use -- FilterModel already
                            // bends it to 20 Hz..Nyquist one layer down,
                            // so a log taper here would double-bend the
                            // same curve.
                            _knobCell(
                                "Cutoff", value: $filterCutoff,
                                range: 0...1, taper: .linear, step: nil,
                                defaultValue: Double(_defaultParams.filterCutoff01),
                                format: "%.2f"
                            )

                            // Resonance only does anything on
                            // S2000/S3000/S3200 (plan section 3.2 item 2
                            // -- the S2000 correction) so it's hidden
                            // rather than shown-but-inert elsewhere.
                            if _filterHasResonance {
                                _knobCell(
                                    "Resonance", value: $filterResonance,
                                    range: 0...1, taper: .linear, step: nil,
                                    defaultValue: Double(_defaultParams.filterResonance01),
                                    format: "%.2f"
                                )
                            }

                            if _stretchIsSupported {
                                // Stretch spans 25..2000% (25..999 on the
                                // S950) -- close to two orders of
                                // magnitude, with the musically useful
                                // region bunched near 100%. A logarithmic
                                // taper gives equal knob rotation to
                                // equal *ratio* change (e.g. 50%->100%
                                // feels like the same twist as
                                // 100%->200%), matching how a
                                // time-stretch amount is actually heard
                                // -- a linear taper would crowd
                                // everything below 200% into a sliver of
                                // the knob's travel.
                                _knobCell(
                                    "Stretch", value: $stretchPercent,
                                    range: 25...max(25.0, Double(machineProfile.maxStretchPercent)),
                                    taper: .logarithmic, step: 1,
                                    defaultValue: Double(_defaultParams.timeFactorPercent),
                                    format: "%.0f%%"
                                )

                                // Cycle length only means anything in
                                // CYCLIC; quality/width only in
                                // INTELLIGENT (plan "2.2", and each
                                // field's own doc comment in
                                // AkaizerCore.h) -- shown accordingly
                                // rather than all-visible-but-some-inert.
                                if _isIntelligentMode {
                                    _knobCell(
                                        "Quality", value: $quality,
                                        range: 0...99, taper: .linear, step: 1,
                                        defaultValue: Double(_defaultParams.quality),
                                        format: "%.0f"
                                    )
                                    _knobCell(
                                        "Width", value: $width,
                                        range: 0...99, taper: .linear, step: 1,
                                        defaultValue: Double(_defaultParams.width),
                                        format: "%.0f"
                                    )
                                } else {
                                    // 20..2000 samples is the same
                                    // span-of-two-orders-of-magnitude
                                    // case as Stretch, for the same
                                    // reason: cycle length is felt/heard
                                    // as a ratio (an octave of grain
                                    // length), not a linear sample count,
                                    // so it gets the same log taper.
                                    _knobCell(
                                        "Cycle", value: $cycleLength,
                                        range: 20...2000, taper: .logarithmic, step: 1,
                                        defaultValue: Double(_defaultParams.cycleLengthSamples),
                                        format: "%.0f smp"
                                    )
                                }
                            }

                            Spacer(minLength: 0)
                        }
                        .padding(.vertical, 2)
                    }

                    if !_stretchIsSupported {
                        // S900 predates the S950's time-stretch feature
                        // entirely (plan section 3.1) -- maxStretchPercent
                        // is 0 for it, which would make 25...0 an invalid
                        // range and crash a Slider (and a knob's own
                        // range-driven maths). Rather than lean on the
                        // `max(25.0, ...)` clamp above alone as the only
                        // thing standing between this and a crash, don't
                        // offer stretch knobs for a machine that
                        // structurally doesn't have the feature.
                        Text("\(machineProfile.displayName) has no time-stretch capability (added in the S950).")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }

                    // Preview -- this IS the "test before committing"
                    // feature: it previews the fully processed result
                    // live, with every knob pushing new params in real
                    // time (isLiveAuditionOn/_startLiveAudition/
                    // _stopLiveAudition below, unchanged). Feedback from
                    // real use was that the old "Live Audition" toggle,
                    // tucked below the knobs, didn't read as that at all
                    // -- promoted to a prominent play/stop button with a
                    // busy light for the one real gap that toggle had
                    // (README's "no recomputing feedback" limitation).
                    HStack(spacing: 8) {
                        Button {
                            isLiveAuditionOn.toggle()
                        } label: {
                            Label(
                                isLiveAuditionOn ? "Stop Preview" : "Preview",
                                systemImage: isLiveAuditionOn ? "stop.fill" : "play.fill"
                            )
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(loadedSample == nil || !_stretchIsSupported)
                        .help("Hear the fully processed result live while you turn the knobs. Nothing is written until you press Process.")

                        if isLiveAuditionOn && isRecomputingVisible {
                            HStack(spacing: 4) {
                                ProgressView().controlSize(.small)
                                Text("recomputing…")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            .transition(.opacity)
                        }
                    }
                    .animation(.default, value: isRecomputingVisible)
                    .onChange(of: isLiveAuditionOn) { _, on in
                        if on { _startLiveAudition() } else { _stopLiveAudition() }
                    }
                    .onReceive(_recomputingPollTimer) { _ in _pollRecomputing() }

                    HStack {
                        Button("Process", action: process)
                            .disabled(loadedSample == nil || !_stretchIsSupported)
                        Button("Revert", action: _revertToOriginal)
                            .disabled(loadedSample == nil || (_snapshot() == .defaults(for: selectedMachine) && processedChannels == nil))
                            .help("Reset all parameters to \(machineProfile.displayName) defaults and discard the processed render. Does not modify the file on disk.")
                        // A/B: "compares processed against dry original
                        // at matched loudness" (plan) -- Play Processed
                        // scales its output to match Play Original's RMS
                        // (see playProcessed), and A/B are keyboard-
                        // toggled per the plan's own wording.
                        Button("Play Original", action: playOriginal)
                            .disabled(loadedSample == nil || isLiveAuditionOn)
                            .keyboardShortcut("a", modifiers: [])
                        Button(_renderIsStale ? "Play Processed (stale)" : "Play Processed", action: playProcessed)
                            .disabled(processedChannels == nil || isLiveAuditionOn)
                            .keyboardShortcut("b", modifiers: [])
                        // One button that stops whichever audio path is
                        // currently active -- offline playback, live
                        // audition, or (in principle, though the rest of
                        // this UI already prevents it) both.
                        Button("Stop", action: _stopAllAudio)
                            .disabled(!isPlayingOffline && !isLiveAuditionOn)
                            .keyboardShortcut(".", modifiers: .command)
                        Button(_renderIsStale ? "Save Processed… (stale)" : "Save Processed…", action: saveProcessed)
                            .disabled(processedChannels == nil)
                        Button("Save Preset…", action: _promptSavePreset)
                            .disabled(loadedSample == nil)
                    }
                }
            }

            Text(statusMessage)
                .font(.callout)
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    /// One rack-panel knob -- see KnobCell.swift for the view itself.
    /// Wires onEditingChanged to ContentView's undo-coalescing pair so
    /// every knob's drag/reset/typed-value interaction becomes one undo
    /// step, without each of the seven call sites below having to know
    /// undo exists.
    private func _knobCell(
        _ label: String, value: Binding<Double>, range: ClosedRange<Double>,
        taper: KnobTaper, step: Double?, defaultValue: Double?, format: String
    ) -> some View {
        KnobCell(
            label: label, value: value, range: range, taper: taper, step: step,
            defaultValue: defaultValue, format: format,
            onEditingChanged: { $0 ? _beginParamEdit() : _endParamEdit() }
        )
    }

    // -- file actions ------------------------------------------------------

    private func openFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.wav, .aiff]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false

        guard panel.runModal() == .OK, let url = panel.url else { return }
        _load(url: url)
    }

    /// Loads a file by URL, whether from the Open panel or the
    /// AKAIZER_AUTOLOAD_PATH env var below.
    private func _load(url: URL) {
        // A running live session is bound to the previous file's channel
        // count and sample rate -- simplest and safest is to stop it
        // rather than try to reconcile those with whatever loads next.
        // Offline playback is bound to the previous file's audio too.
        isLiveAuditionOn = false
        _stopLiveAudition()
        playback.stop()
        isPlayingOffline = false

        do {
            let sample = try audioFileService.load(url: url)
            loadedSample = sample
            statusMessage = "Loaded \(url.lastPathComponent)."
            lastVerifyResult = nil
            processedChannels = nil
            processedWaveformSamples = nil
            renderedSnapshot = nil
            _addToRecentFiles(url)

            let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
            let channels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)
            originalWaveformSamples = channels.first ?? []
        } catch {
            statusMessage = "Failed to load: \(error)"
        }
    }

    private func _addToRecentFiles(_ url: URL) {
        recentFiles.removeAll { $0 == url }
        recentFiles.insert(url, at: 0)
        if recentFiles.count > maxRecentFiles {
            recentFiles.removeLast(recentFiles.count - maxRecentFiles)
        }
    }

    /// Loads AKAIZER_AUTOLOAD_PATH on launch if set. Exists so the app's
    /// real audio behaviour can be driven and verified (via `swift run`
    /// or the built binary) without going through NSOpenPanel, whose
    /// accessibility tree has proven unreliable to automate in a
    /// sandboxed terminal -- clicks on its column browser rows and
    /// synthetic keystrokes into its fields didn't register, even though
    /// every other control in this app's own window did. Harmless for
    /// normal use: only fires when that specific env var is set.
    private func _autoloadIfRequested() {
        guard let path = ProcessInfo.processInfo.environment["AKAIZER_AUTOLOAD_PATH"] else { return }
        _load(url: URL(fileURLWithPath: path))
    }

    // -- drag-and-drop file loading ------------------------------------------

    /// NSItemProvider's URL retrieval is async, but onDrop's `perform`
    /// must answer synchronously whether the drop is accepted -- return
    /// true here (the drag conforms to one of our accepted types) and do
    /// the actual load once the file resolves.
    ///
    /// loadFileRepresentation(forTypeIdentifier:), not a public.file-url
    /// lookup -- diagnostic logging showed a real dragged item registering
    /// ONLY its concrete content type ("com.microsoft.waveform-audio"),
    /// with no "public.file-url" representation at all: this drag source
    /// vends the file's raw bytes directly, not a URL reference, so
    /// anything that only asked for public.file-url was guaranteed to
    /// reject it. loadFileRepresentation handles both cases uniformly --
    /// for a real file-url-backed provider it hands back that file
    /// directly; for a data-backed one (this case) the system writes the
    /// bytes to a temp file first and hands back that instead. Either
    /// way the URL is valid only for the duration of the completion
    /// closure (the system deletes its temp copy right after), so it's
    /// copied into our own temp location before returning to the main
    /// actor.
    private func _handleDrop(providers: [NSItemProvider]) -> Bool {
        guard let provider = providers.first else { return false }
        let candidateTypes = [UTType.wav.identifier, UTType.aiff.identifier, UTType.fileURL.identifier]
        guard let typeID = candidateTypes.first(where: { provider.hasItemConformingToTypeIdentifier($0) }) else {
            return false
        }

        provider.loadFileRepresentation(forTypeIdentifier: typeID) { url, error in
            guard let url else {
                DispatchQueue.main.async {
                    statusMessage = "Couldn't read the dropped file" + (error.map { ": \($0.localizedDescription)" } ?? ".")
                }
                return
            }
            do {
                // Copy synchronously, inside this closure -- url itself
                // stops being valid the moment it returns.
                let stagingDir = FileManager.default.temporaryDirectory
                    .appendingPathComponent("Patina-Drop", isDirectory: true)
                    .appendingPathComponent(UUID().uuidString, isDirectory: true)
                try FileManager.default.createDirectory(at: stagingDir, withIntermediateDirectories: true)
                let destination = stagingDir.appendingPathComponent(url.lastPathComponent)
                try FileManager.default.copyItem(at: url, to: destination)
                DispatchQueue.main.async { _loadDroppedURL(destination) }
            } catch {
                DispatchQueue.main.async {
                    statusMessage = "Couldn't read the dropped file: \(error.localizedDescription)"
                }
            }
        }
        return true
    }

    private func _loadDroppedURL(_ url: URL) {
        // Ignore the app's own drag-out export temp files -- without
        // this, dragging the waveform out and back into the same window
        // would re-import the just-rendered copy as if it were a
        // brand-new sample.
        let dragTempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("Patina-Drag", isDirectory: true)
        guard !url.path.hasPrefix(dragTempDir.path) else {
            return
        }
        _load(url: url)
    }

    /// Deletes leftover temp files from previous runs -- both the
    /// drag-OUT export staging dir (ProcessedWavExport.swift; there's no
    /// reliable "the receiver finished copying" callback through
    /// .draggable, so cleanup is a sweep on next launch rather than
    /// delete-on-completion) and the drag-IN staging dir (_handleDrop
    /// above, for drag sources that vend raw content instead of a
    /// public.file-url). Worst case either way is a harmless leak in the
    /// OS temp dir until this runs again.
    private func _sweepDragExportTempFiles() {
        for name in ["Patina-Drag", "Patina-Drop"] {
            let dir = FileManager.default.temporaryDirectory.appendingPathComponent(name, isDirectory: true)
            try? FileManager.default.removeItem(at: dir)
        }
    }

    private func saveCopy() {
        guard let sample = loadedSample else { return }
        _presentSavePanel(for: sample.url, suffix: "-copy") { url in
            try audioFileService.save(sample, to: url)
            statusMessage = "Saved unchanged copy to \(url.lastPathComponent)."
        }
    }

    private func verifyRoundTrip() {
        guard let sample = loadedSample else { return }

        let tempURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(sample.url.pathExtension)

        do {
            try audioFileService.save(sample, to: tempURL)
            let reloaded = try audioFileService.load(url: tempURL)
            try? FileManager.default.removeItem(at: tempURL)

            let identical = reloaded.rawData == sample.rawData && reloaded.format == sample.format
            lastVerifyResult = identical
                ? "✓ Round trip is bit-identical (\(sample.frameCount) frames)."
                : "✗ Round trip differs."
        } catch {
            lastVerifyResult = "✗ Round trip failed: \(error)"
        }
    }

    // -- stretch actions -----------------------------------------------------

    /// Reads the ten parameter @State vars into one comparable/undoable
    /// value. The single place both _currentParams() and every bulk-write
    /// path (undo, revert, preset apply) read from or compare against.
    private func _snapshot() -> ParamSnapshot {
        ParamSnapshot(
            machine: selectedMachine, engine: selectedEngine, mode: selectedMode,
            stretchPercent: stretchPercent, cycleLength: cycleLength,
            quality: quality, width: width, transposeSemitones: transposeSemitones,
            filterCutoff: filterCutoff, filterResonance: filterResonance
        )
    }

    /// Builds params from the current picker/slider state -- the single
    /// place both the offline Process button and live audition read from,
    /// so they can never drift apart.
    private func _currentParams() -> AkzStretchParams {
        _snapshot().params
    }

    /// True once processedChannels no longer matches the params it was
    /// rendered with. Deliberately never used to disable anything -- a
    /// stale render is still valid audio -- only to mark Play/Save
    /// Processed and to decide whether a drag-out export needs to
    /// re-render first.
    private var _renderIsStale: Bool {
        processedChannels != nil && renderedSnapshot != _snapshot()
    }

    // -- undo/redo (params only) ---------------------------------------------

    private func _pushUndo(_ s: ParamSnapshot) {
        undoStack.append(s)
        if undoStack.count > maxUndoDepth {
            undoStack.removeFirst(undoStack.count - maxUndoDepth)
        }
        redoStack.removeAll()
    }

    /// Opens a coalescing transaction. Closes any already-open one first
    /// (_endParamEdit) so a dropped onEnded -- e.g. the window losing
    /// focus mid-drag -- gets folded into the next edit instead of
    /// silently discarding an undo step.
    private func _beginParamEdit() {
        _endParamEdit()
        pendingEditSnapshot = _snapshot()
    }

    private func _endParamEdit() {
        guard let before = pendingEditSnapshot else { return }
        pendingEditSnapshot = nil
        guard before != _snapshot() else { return } // a drag that changed nothing -> no step
        _pushUndo(before)
    }

    private func _undo() {
        _endParamEdit()
        guard let previous = undoStack.popLast() else { return }
        redoStack.append(_snapshot())
        _applySnapshot(previous)
        statusMessage = "Undid parameter change."
    }

    private func _redo() {
        _endParamEdit()
        guard let next = redoStack.popLast() else { return }
        undoStack.append(_snapshot())
        _applySnapshot(next)
        statusMessage = "Redid parameter change."
    }

    /// Wraps a discrete control's own binding (Engine, Mode) so its write
    /// pushes an undo step. Unlike a knob drag there's no gesture to
    /// bracket with onEditingChanged, and the control's own onChange
    /// fires after the write -- so the push happens here, at the point of
    /// assignment, using the state read just before it.
    private func _undoableBinding<T: Equatable>(
        get: @escaping () -> T, set: @escaping (T) -> Void
    ) -> Binding<T> {
        Binding(
            get: get,
            set: { newValue in
                guard newValue != get() else { return }
                _pushUndo(_snapshot())
                set(newValue)
            }
        )
    }

    /// The Machine picker's binding setter, not an .onChange -- .onChange
    /// runs during the view-update pass *after* state has already
    /// settled, not synchronously at the point of assignment, so it can't
    /// be raced against a bulk write that also touches selectedMachine
    /// (undo restore, preset apply, revert): whichever one's onChange
    /// fires later would silently clobber the other. Doing the "new
    /// machine -> reset everything else" work here instead, synchronously,
    /// means bulk writes go through _applySnapshot() below and never
    /// trigger this at all -- there's nothing left to race.
    private func _selectMachine(_ machine: AkzMachine) {
        guard machine != selectedMachine else { return }
        _pushUndo(_snapshot())
        selectedMachine = machine
        let defaults = ParamSnapshot.defaults(for: machine)
        stretchPercent = defaults.stretchPercent
        cycleLength = defaults.cycleLength
        quality = defaults.quality
        width = defaults.width
        transposeSemitones = defaults.transposeSemitones
        filterCutoff = defaults.filterCutoff
        filterResonance = defaults.filterResonance
        // S950 has no CYCLIC/INTELLIGENT switch at all (Mon1/Pol2
        // instead -- plan section 3.2). Force the picker back to a real
        // state rather than silently ignoring a stale "Intelligent"
        // selection the engine itself would ignore too.
        if StretchProcessor.profile(for: machine).hasModeSwitch == 0 {
            selectedMode = AkzStretchMode_Cyclic
        }
        if StretchProcessor.profile(for: machine).supportsTimeStretch == 0 {
            isLiveAuditionOn = false // triggers its own onChange -> _stopLiveAudition()
        } else {
            _pushLiveParamsIfNeeded()
        }
    }

    /// Assigns all ten params from a snapshot in one shot -- undo restore,
    /// revert, and preset apply all funnel through this. Assigning
    /// selectedMachine directly here (never through _selectMachine) is
    /// what keeps a bulk write from triggering the machine-change reset
    /// above and clobbering the very values being restored.
    private func _applySnapshot(_ s: ParamSnapshot) {
        selectedMachine = s.machine
        selectedEngine = s.engine
        selectedMode = s.mode
        stretchPercent = s.stretchPercent
        cycleLength = s.cycleLength
        quality = s.quality
        width = s.width
        transposeSemitones = s.transposeSemitones
        filterCutoff = s.filterCutoff
        filterResonance = s.filterResonance
        _pushLiveParamsIfNeeded()
    }

    // -- presets ---------------------------------------------------------------

    private func _promptSavePreset() {
        let alert = NSAlert()
        alert.messageText = "Save Preset"
        alert.informativeText = "Name this parameter set."
        alert.addButton(withTitle: "Save")
        alert.addButton(withTitle: "Cancel")

        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        field.placeholderString = "e.g. Jungle S950"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field

        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let name = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return }

        let preset = AkaizerPreset(name: name, params: _currentParams())
        presets.removeAll { $0.name == name } // overwrite a same-name preset rather than duplicate it
        presets.append(preset)
        presetStore.save(presets)
        statusMessage = "Saved preset \"\(name)\"."
    }

    /// Applies a preset's params to every slider/picker via
    /// _applySnapshot(), which assigns selectedMachine directly rather
    /// than through _selectMachine -- so the machine-change reset never
    /// fires and the preset's own values (including its machine) are what
    /// stick, with no assignment-order dependency to get right.
    private func _applyPreset(_ preset: AkaizerPreset) {
        _pushUndo(_snapshot())
        _applySnapshot(preset.snapshot)
        statusMessage = "Applied preset \"\(preset.name)\"."
    }

    private func _deletePreset(_ preset: AkaizerPreset) {
        presets.removeAll { $0.id == preset.id }
        presetStore.save(presets)
    }

    // -- revert ------------------------------------------------------------

    /// The app is already non-destructive -- loadedSample.rawData is
    /// never mutated, and process() re-decodes from it every call -- so
    /// "revert to original" means discarding the *edit state*, not
    /// restoring a file. Keeps the current machine (it's closer to a
    /// document mode than a parameter -- silently jumping to a different
    /// sampler would be startling) and resets the other nine params to
    /// that machine's defaults, then discards the render. One undo step;
    /// undoing it restores the params but not the discarded render --
    /// that's the honest consequence of undo covering parameters only,
    /// not something worth papering over by putting [[Float]] buffers in
    /// the undo stack.
    private func _revertToOriginal() {
        _endParamEdit()
        _pushUndo(_snapshot())
        _applySnapshot(.defaults(for: selectedMachine))
        processedChannels = nil
        processedWaveformSamples = nil
        renderedSnapshot = nil
        statusMessage = "Reverted to \(machineProfile.displayName) defaults."
    }

    // -- recomputing busy light ---------------------------------------------

    private func _pollRecomputing() {
        let now = Date()
        let raw = liveController?.isRecomputing ?? false

        if raw {
            if _recomputingBusySince == nil { _recomputingBusySince = now }
            if !isRecomputingVisible, let since = _recomputingBusySince, now.timeIntervalSince(since) >= 0.15 {
                isRecomputingVisible = true
                _recomputingVisibleUntil = now.addingTimeInterval(0.25)
            }
        } else {
            _recomputingBusySince = nil
            if isRecomputingVisible, let until = _recomputingVisibleUntil, now >= until {
                isRecomputingVisible = false
                _recomputingVisibleUntil = nil
            }
        }
    }

    /// Stops whichever of the two independent audio engines
    /// (AudioPlaybackController's, LiveAuditionController's) is
    /// currently running. The two are never meant to be heard together --
    /// this is both the Stop button's action and the guard every path
    /// that starts one of them calls first.
    private func _stopAllAudio() {
        playback.stop()
        isPlayingOffline = false
        isLiveAuditionOn = false // onChange(of: isLiveAuditionOn) below calls _stopLiveAudition()
    }

    private func _startLiveAudition() {
        guard let sample = loadedSample else { return }
        // AudioPlaybackController and LiveAuditionController each own
        // their own AVAudioEngine -- starting this one has never stopped
        // the other, which is exactly the reported "plays multiple
        // versions... on top of one another" bug.
        playback.stop()
        isPlayingOffline = false
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let channels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)

        let controller = LiveAuditionController(channelCount: sample.channelCount, sampleRateHz: sample.sampleRateHz)
        controller.setSource(channels: channels)
        controller.setParams(_currentParams())
        do {
            try controller.start()
            liveController = controller
            statusMessage = "Live audition running -- drag Stretch/Cycle to hear changes."
        } catch {
            statusMessage = "Live audition failed to start: \(error)"
            isLiveAuditionOn = false
        }
    }

    private func _stopLiveAudition() {
        liveController?.stop()
        liveController = nil
    }

    /// Called from every param-affecting onChange handler. A no-op
    /// unless live audition is actually running, so normal offline use
    /// (Process/Save) pays nothing for this.
    private func _pushLiveParamsIfNeeded() {
        guard isLiveAuditionOn, let controller = liveController else { return }
        controller.setParams(_currentParams())
    }

    private func process() {
        guard let sample = loadedSample else { return }

        let snapshot = _snapshot()
        let outputChannels = ProcessedRender.render(sample: sample, params: snapshot.params)

        processedChannels = outputChannels
        processedWaveformSamples = outputChannels.first
        renderedSnapshot = snapshot
        let outFrames = outputChannels.first?.count ?? 0
        statusMessage = "Processed: \(sample.frameCount) → \(outFrames) frames (\(String(format: "%.2f", Double(outFrames) / sample.sampleRateHz))s)."
    }

    private func playOriginal() {
        guard let sample = loadedSample else { return }
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let channels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)
        do {
            try playback.play(channels: channels, sampleRateHz: sample.sampleRateHz)
            isPlayingOffline = true
        } catch {
            statusMessage = "Playback failed: \(error)"
        }
    }

    private func playProcessed() {
        guard let sample = loadedSample, let channels = processedChannels else { return }

        // Loudness-matched A/B (plan section 5/8): scale the processed
        // render to the ORIGINAL's RMS, so a level difference between
        // the two never gets mistaken for the stretch effect itself.
        // Play Original is the reference and always plays unscaled.
        let originalInterleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let originalChannels = PCMConversion.deinterleave(originalInterleaved, channelCount: sample.channelCount)
        let referenceRMS = PCMConversion.rms(originalChannels)
        let gain = PCMConversion.matchedGain(channels, toMatchRMS: referenceRMS)
        let matchedChannels = PCMConversion.applyGain(channels, gain: gain)

        do {
            try playback.play(channels: matchedChannels, sampleRateHz: sample.sampleRateHz)
            isPlayingOffline = true
        } catch {
            statusMessage = "Playback failed: \(error)"
        }
    }

    private func saveProcessed() {
        guard let sample = loadedSample, let channels = processedChannels else { return }
        _presentSavePanel(for: sample.url, suffix: "-stretched") { url in
            let interleaved = PCMConversion.interleave(channels)
            let rawData = PCMConversion.fromFloat(interleaved, format: sample.format)
            let processedSample = LoadedSample(url: url, format: sample.format, rawData: rawData)
            try audioFileService.save(processedSample, to: url)
            statusMessage = "Saved processed audio to \(url.lastPathComponent)."
        }
    }

    /// Builds the value behind WaveformView's .draggable() -- see
    /// ProcessedWavExport.swift's header for the full rationale. Cheap by
    /// design: hands over the current snapshot/cached-channels rather
    /// than doing any rendering here, so this can be called fresh on
    /// every body evaluation without cost.
    private func _dragExport(for sample: LoadedSample) -> ProcessedWavExport {
        ProcessedWavExport(
            source: sample,
            snapshot: _snapshot(),
            cachedChannels: _renderIsStale ? nil : processedChannels,
            fileName: sample.url.deletingPathExtension().lastPathComponent + "-stretched.wav",
            onRendered: { channels, snapshot in
                // ProcessedWavExport invokes this via `await
                // MainActor.run`, so it's genuinely always on the main
                // actor -- assumeIsolated tells the compiler that rather
                // than hopping again, since the @Sendable closure type
                // (required to cross into the async export path) can't
                // itself express that guarantee.
                MainActor.assumeIsolated {
                    processedChannels = channels
                    processedWaveformSamples = channels.first
                    renderedSnapshot = snapshot
                }
            }
        )
    }

    private func _presentSavePanel(for sourceURL: URL, suffix: String, action: (URL) throws -> Void) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [sourceURL.pathExtension.lowercased() == "aiff" ? .aiff : .wav]
        panel.nameFieldStringValue = sourceURL.deletingPathExtension().lastPathComponent + suffix + "." + sourceURL.pathExtension

        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try action(url)
        } catch {
            statusMessage = "Failed: \(error)"
        }
    }
}
