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

    /// Shared by selectedMachine's and sampleRateHz's own initialisers
    /// below, so the two can't drift: sampleRateHz's default must always
    /// be THIS machine's own top-end rate, not some other machine's.
    private static let initialMachine: AkzMachine = AkzMachine_S950

    @State private var selectedMachine: AkzMachine = Self.initialMachine
    @State private var selectedEngine: AkzEngine = AkzEngine_Classic
    @State private var selectedMode: AkzStretchMode = AkzStretchMode_Cyclic
    @State private var stretchPercent: Double = 100
    @State private var cycleLength: Double = 1000
    @State private var quality: Double = 10
    @State private var width: Double = 10
    @State private var transposeSemitones: Double = 0
    @State private var filterCutoff: Double = 1.0
    @State private var filterResonance: Double = 0.0
    /// The bandwidth knob's rate, in Hz -- always real and in-range,
    /// never 0/bypass (2.1 feedback: "this is the essence of the old
    /// sampler sound"). Defaults to initialMachine's own top-end rate,
    /// same as ParamSnapshot.defaults(for:) would give it.
    @State private var sampleRateHz: Double = ParamSnapshot.defaults(for: Self.initialMachine).sampleRateHz
    @State private var processedChannels: [[Float]]?
    /// True whenever `processedChannels` holds a render that hasn't been
    /// written to disk yet (Save Processed/Save Preview, or a completed
    /// drag-out export) -- 2.3.2 feedback: closing the window now quits
    /// the whole app (see PatinaApp's `applicationShouldTerminateAfter
    /// LastWindowClosed`), so losing an unsaved render on an accidental
    /// close/quit needs a real "are you sure" gate, not just an assumption
    /// nothing valuable was ever in flight.
    ///
    /// A plain proxy onto `QuitGuard.shared`, not its own `@State` --
    /// `AppDelegate` (PatinaApp.swift) is a plain NSObject, not a SwiftUI
    /// View, so it has no way to read this struct's @State directly, and
    /// nothing in THIS view's body renders differently based on the flag
    /// (it only gates termination), so there's no observation to lose by
    /// not using @State.
    private var hasUnsavedProcessedAudio: Bool {
        get { QuitGuard.shared.hasUnsavedProcessedAudio }
        nonmutating set { QuitGuard.shared.hasUnsavedProcessedAudio = newValue }
    }

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
    /// explicitly ruled out. Persisted via RecentFilesStore since v2
    /// (was session-only in v1).
    @State private var recentFiles: [URL] = []
    private let maxRecentFiles = 8

    /// Named parameter sets -- "Jungle S950," "Dusty S1000," the plan's
    /// own examples. Persisted via PresetStore, loaded once on launch.
    @State private var presets: [AkaizerPreset] = []

    /// Filters the sidebar's MACHINES section (v2 heritage-roster plan,
    /// stage 9) -- only shown once the roster is long enough to need it.
    @State private var machineSearchText: String = ""

    /// Mono (channel 0) traces for WaveformView. Decoded once per load/
    /// process rather than in the view body, so scrolling/resizing the
    /// window doesn't re-decode a whole file's worth of PCM every frame.
    @State private var originalWaveformSamples: [Float] = []
    @State private var processedWaveformSamples: [Float]?

    // -- waveform transport (2.1 feedback: "show playback bar over
    // sample waveform... click and move start point with mouse") -------

    /// Which of the app's independent audio sources is currently
    /// playing, if any -- what _pollTransport reads playheadFraction
    /// from. Distinct from isPlayingOffline/isLiveAuditionOn (which
    /// exist for button-enabled state): this exists so the poll knows
    /// WHICH controller's position to ask for.
    private enum PlayingSource { case none, original, processed, live }
    @State private var playingSource: PlayingSource = .none

    /// [0, 1) fraction through the current source's playback, or nil
    /// when nothing is playing -- polled from AudioPlaybackController/
    /// LiveAuditionController by _pollTransport, same cadence as the
    /// existing recomputing-busy-light poll.
    @State private var playheadFraction: Double?

    /// Frame index (into the ORIGINAL, untrimmed sample) playback and
    /// rendering start from -- a transport position, not a param (see
    /// ProcessedRender.render's own doc comment on why this never
    /// touches AkzStretchParams/ParamSnapshot). Reset to 0 on every load;
    /// dragged via the waveform's start-point marker.
    @State private var startFrame: Int = 0

    /// The startFrame the current processedChannels render actually used
    /// -- extends _renderIsStale the same way renderedSnapshot does for
    /// the ten DSP params.
    @State private var renderedStartFrame: Int?

    private let audioFileService = AudioFileService()
    private let playback = AudioPlaybackController()
    private let presetStore = PresetStore()
    private let recentFilesStore = RecentFilesStore(maxCount: 8)

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

    /// "1.2.3 (45)" from Info.plist's CFBundleShortVersionString/
    /// CFBundleVersion -- [user feedback, 2026-09]: no version number
    /// was shown anywhere in the UI. Falls back to just "?" for either
    /// half if Info.plist is somehow missing it (e.g. a non-bundled
    /// debug build), rather than showing nothing or crashing.
    private var _appVersionString: String {
        let info = Bundle.main.infoDictionary
        let shortVersion = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "v\(shortVersion) (\(build))"
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

        // Bandwidth (v2 heritage-roster plan, stage 4/9) -- only on a
        // machine whose sample rate is genuinely a knob, not a fixed
        // spec. sampleRateHz is always a real, in-range value as of 2.1
        // (never the old 0/bypass sentinel), so this is just the value
        // -- plus a state word explaining WHY the top of the knob's
        // travel can sound unprocessed: 2.1 feedback was that the
        // control needed to be "more user friendly," and "the number
        // changed but the sound didn't" is the actual illegibility, not
        // a bug in the rate stage (you can't decimate a file UP to a
        // rate above its own, so a machine oversampling the loaded file
        // is a genuine, honest no-op -- see RateModel.cpp).
        if machineProfile.hasVariableSampleRate != 0 {
            var rateText = "\(Int(sampleRateHz))hz"
            if sampleRateHz >= machineProfile.maxSampleRateHz {
                rateText += " · full"
            } else if let loadedSample, sampleRateHz >= loadedSample.sampleRateHz {
                rateText += " · at source rate"
            }
            rows.append([LCDField("rate", rateText)])
        }

        if _inferredStageCount > 0 {
            rows.append([LCDField("modelled", "\(_citedStageCount) cited · \(_inferredStageCount) inferred")])
        }

        return rows
    }

    // -- provenance (v2 heritage-roster plan, stage 9) ------------------------

    /// Stages whose provenance for the current machine is Inferred or
    /// Unmodelled -- the "must be visible in the UI" half of v2's
    /// fidelity bar (citation-first, inference allowed only if flagged).
    private var _inferredStageCount: Int {
        StretchProcessor.allStages.filter {
            let level = StretchProcessor.provenance(for: selectedMachine, stage: $0).level
            return level == AkzProvenanceLevel_Inferred || level == AkzProvenanceLevel_Unmodelled
        }.count
    }

    private var _citedStageCount: Int {
        StretchProcessor.allStages.count - _inferredStageCount
    }

    /// Read-only machine identity + capability chips + the "Modelled
    /// from..." provenance panel. Replaces the Machine Picker that used
    /// to live here -- selection itself moved to the sidebar.
    private var _machineHeader: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text(machineProfile.displayName)
                    .font(.headline)
                // String(year), not raw Int interpolation -- Text's
                // LocalizedStringKey interpolation formats an
                // interpolated Int with locale grouping by default
                // ("1,988"), which reads as a quantity, not a year.
                Text("\(machineProfile.manufacturerName) · \(String(machineProfile.yearIntroduced))")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                Spacer()
                if _stretchIsSupported {
                    _capabilityChip("Stretch")
                }
                if _filterHasResonance {
                    _capabilityChip("Resonance")
                }
                if machineProfile.hasVariableSampleRate != 0 {
                    _capabilityChip("Bandwidth")
                }
            }

            DisclosureGroup("Modelled from…") {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(StretchProcessor.allStages, id: \.rawValue) { stage in
                        _provenanceRow(stage)
                    }
                }
                .padding(.top, 4)
            }
            .font(.caption)
        }
    }

    private func _capabilityChip(_ text: String) -> some View {
        Text(text)
            .font(.caption2.weight(.medium))
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Color.accentColor.opacity(0.15), in: Capsule())
            .foregroundStyle(Color.accentColor)
    }

    private func _provenanceRow(_ stage: AkzStage) -> some View {
        let (level, note) = StretchProcessor.provenance(for: selectedMachine, stage: stage)
        let isInferred = level == AkzProvenanceLevel_Inferred || level == AkzProvenanceLevel_Unmodelled
        return HStack(alignment: .top, spacing: 6) {
            Text(_provenanceBadge(level))
                .font(.caption2.weight(.bold))
                .foregroundStyle(isInferred ? .orange : .secondary)
                .frame(width: 60, alignment: .leading)
            VStack(alignment: .leading, spacing: 1) {
                Text(StretchProcessor.label(for: stage))
                    .font(.caption.weight(.medium))
                Text(note)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func _provenanceBadge(_ level: AkzProvenanceLevel) -> String {
        switch level {
        case AkzProvenanceLevel_Measured: return "MEASURED"
        case AkzProvenanceLevel_Manual: return "CITED"
        case AkzProvenanceLevel_Inferred: return "INFERRED"
        case AkzProvenanceLevel_Unmodelled: return "N/A"
        default: return "?"
        }
    }

    var body: some View {
        HStack(spacing: 0) {
            _sidebar
            Divider()
            _mainContent
        }
        .frame(minWidth: 720, minHeight: 600)
        // 2.3.2: installs QuitAwareWindowDelegate on the real NSWindow --
        // see PatinaApp.swift's WindowCloseGate/QuitAwareWindowDelegate
        // for why the red close button needs its own gate, separate from
        // applicationShouldTerminate's Cmd+Q gate.
        .withQuitAwareWindowClose()
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
            let (loadedPresets, presetError) = presetStore.loadOrRecover()
            presets = loadedPresets
            recentFiles = recentFilesStore.load()
            if let presetError {
                statusMessage = presetError
            }
            playback.onFinished = {
                isPlayingOffline = false
                if playingSource == .original || playingSource == .processed { playingSource = .none }
            }
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

    /// Machines filtered by `machineSearchText`, grouped by manufacturer
    /// -- v2 heritage-roster plan, stage 9: replaces the flat Machine
    /// Picker once the roster grows past six. Groups are ordered by
    /// each manufacturer's first appearance in
    /// StretchProcessor.allMachines (itself AkzMachine's declaration
    /// order), not alphabetically -- Akai first, matching the app's own
    /// history.
    private var _groupedMachines: [(manufacturer: String, machines: [AkzMachine])] {
        let filtered = StretchProcessor.allMachines.filter { machine in
            guard !machineSearchText.isEmpty else { return true }
            let profile = StretchProcessor.profile(for: machine)
            return profile.displayName.localizedCaseInsensitiveContains(machineSearchText)
                || profile.manufacturerName.localizedCaseInsensitiveContains(machineSearchText)
        }
        var order: [String] = []
        var groups: [String: [AkzMachine]] = [:]
        for machine in filtered {
            let manufacturer = StretchProcessor.profile(for: machine).manufacturerName
            if groups[manufacturer] == nil { order.append(manufacturer) }
            groups[manufacturer, default: []].append(machine)
        }
        return order.map { (manufacturer: $0, machines: groups[$0] ?? []) }
    }

    private var _sidebar: some View {
        ScrollView {
        VStack(alignment: .leading, spacing: 2) {
            Text("MACHINES")
                .font(.caption.weight(.semibold))
                .tracking(1.0)
                .foregroundStyle(.secondary)
                .padding(.top, 16)
                .padding(.horizontal, 14)
                .padding(.bottom, 6)

            // Only worth the pixels once the roster is long enough to
            // need it -- a fixed threshold rather than always-on, since
            // six machines fit on screen with room to spare.
            if StretchProcessor.allMachines.count > 8 {
                TextField("Filter…", text: $machineSearchText)
                    .textFieldStyle(.roundedBorder)
                    .font(.caption)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 4)
            }

            ForEach(_groupedMachines, id: \.manufacturer) { group in
                Text(group.manufacturer.uppercased())
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(.tertiary)
                    .padding(.horizontal, 14)
                    .padding(.top, 4)
                ForEach(group.machines, id: \.rawValue) { machine in
                    _machineRow(machine)
                }
            }

            HStack {
                Text("RECENT")
                    .font(.caption.weight(.semibold))
                    .tracking(1.0)
                    .foregroundStyle(.secondary)
                if !recentFiles.isEmpty {
                    Spacer()
                    Button("Clear", action: _clearRecentFiles)
                        .buttonStyle(.plain)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .help("Remove every entry from the Recent list. Doesn't touch the files themselves.")
                }
            }
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

        }
        .padding(.bottom, 16)
        }
        .frame(width: 220)
        .frame(maxHeight: .infinity)
        .background(Color(nsColor: .controlBackgroundColor))
    }

    private func _machineRow(_ machine: AkzMachine) -> some View {
        let isSelected = machine == selectedMachine
        return Button(action: { _selectMachine(machine) }) {
            HStack(spacing: 7) {
                Circle()
                    .fill(isSelected ? Color.accentColor : .clear)
                    .frame(width: 6, height: 6)
                Text(StretchProcessor.profile(for: machine).displayName)
                    .font(.callout)
                    .lineLimit(1)
                    .foregroundStyle(isSelected ? Color.primary : Color.secondary)
                Spacer()
                Text(String(StretchProcessor.profile(for: machine).yearIntroduced)) // not raw Int interpolation -- see _machineHeader's comment
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 5)
        }
        .buttonStyle(.plain)
        .background(isSelected ? Color.accentColor.opacity(0.12) : Color.clear)
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
                let referenceCount = max(originalWaveformSamples.count, processedWaveformSamples?.count ?? 0)
                WaveformView(
                    samples: originalWaveformSamples,
                    overlaySamples: processedWaveformSamples,
                    startFraction: referenceCount > 0 ? Double(startFrame) / Double(referenceCount) : 0,
                    playheadFraction: _playheadSharedFraction,
                    dragExport: _dragExport(for: sample),
                    onScrubEnded: { fraction in
                        startFrame = Int((fraction * Double(referenceCount)).rounded())
                        _pushLiveSourceIfNeeded()
                    }
                )
                .help("Click and drag to export the processed audio · ⇧-click to set the start point")
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
                    // Machine selection moved to the sidebar's MACHINES
                    // section (v2 heritage-roster plan, stage 9) once a
                    // flat Picker stopped scaling past six -- this is
                    // now a read-only header plus the provenance panel,
                    // not a second selection surface. The live-param
                    // push chain (unchanged mechanism -- attaching to
                    // ANY mounted view in this subtree behaves
                    // identically) moves here since the Picker it used
                    // to ride on is gone.
                    _machineHeader
                        .onChange(of: selectedEngine) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: selectedMode) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: stretchPercent) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: cycleLength) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: quality) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: width) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: transposeSemitones) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: filterCutoff) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: filterResonance) { _, _ in _pushLiveParamsIfNeeded() }
                        .onChange(of: sampleRateHz) { _, _ in _pushLiveParamsIfNeeded() }

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
                    //
                    // Generated from MachineControls.controls(for:mode:)
                    // (v2 heritage-roster plan, stage 9) rather than a
                    // hand-written chain of `if`s -- SP-1200 (Cutoff +
                    // Transpose only, no stretch or bandwidth cluster at
                    // all) and S950 (bandwidth AND stretch together) are
                    // orthogonal axes that used to multiply combinations
                    // by hand; the descriptor table gets every
                    // combination right by construction instead.
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(alignment: .top, spacing: 20) {
                            ForEach(MachineControls.controls(for: selectedMachine, mode: selectedMode), id: \.id) { descriptor in
                                _knobCell(
                                    descriptor.label, value: _binding(for: descriptor.id),
                                    range: descriptor.range, taper: descriptor.taper, step: descriptor.step,
                                    defaultValue: _defaultValue(for: descriptor.id),
                                    format: descriptor.format
                                )
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
                        // "(added in the S950)" only makes sense for
                        // S900 -- the four heritage-roster machines
                        // (v2 stage 10) never had time-stretch at all,
                        // so the notice is machine-specific rather than
                        // always naming the S950.
                        Text(selectedMachine == AkzMachine_S900
                            ? "\(machineProfile.displayName) has no time-stretch capability (added in the S950)."
                            : "\(machineProfile.displayName) has no time-stretch capability -- Preview/Process still apply its converter, filter and varispeed character.")
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
                        // No `|| !_stretchIsSupported` here (v2
                        // heritage-roster plan, stage 11 fix): every
                        // machine has SOME audio-affecting parameters
                        // (Transpose, Cutoff, and now Bandwidth are all
                        // unconditional -- MachineControls.swift), not
                        // just the ones that time-stretch. Disabling
                        // Preview/Process for a no-stretch machine
                        // predates v2 (it also silently blocked S900),
                        // but four of the ten machines here have NO
                        // time-stretch at all -- leaving it in would
                        // have meant SP-1200/Fairlight/Mirage/Emulator
                        // II could show knobs a user could never
                        // actually hear or export. StretchEngine.cpp's
                        // own supportsTimeStretch gate (stage 3) already
                        // forces ratio 1.0 for these machines regardless
                        // of what the UI sends, so processing one is
                        // always safe.
                        .disabled(loadedSample == nil)
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
                    .onReceive(_recomputingPollTimer) { _ in _pollTransport() }

                    HStack {
                        Button("Process", action: process)
                            .disabled(loadedSample == nil) // see the Preview button's comment above -- every machine has something to process, not just stretch-capable ones
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
                        // 2.3 feedback: chain repeated stretches/filters
                        // the way the real hardware's own re-sampling
                        // workflow did (stretch, then stretch the result
                        // again). In-memory only -- Save Processed above
                        // is the path to keep a generation on disk.
                        Button("Use as Source", action: _useProcessedAsSource)
                            .disabled(processedChannels == nil)
                            .help("Make the processed render the new source, in place -- chain another pass of stretch/filter on top of it.")
                        // v2 heritage-roster plan, stage 11: closes the
                        // README's "no 'save what I'm hearing right
                        // now' path" gap. Save Processed above saves the
                        // last offline Process() result, which can
                        // differ from live audition's current sound if
                        // a knob moved since the last Process press --
                        // this renders fresh from the CURRENT params
                        // (the same ones live audition is already
                        // playing, via _currentParams()) without
                        // stopping audition, so what gets saved is
                        // provably what's audible right now.
                        Button("Save Preview…", action: _saveWhatImHearing)
                            .disabled(loadedSample == nil || !isLiveAuditionOn)
                            .help("Render and save exactly what live audition is currently playing, without stopping it.")
                        Button("Save Preset…", action: _promptSavePreset)
                            .disabled(loadedSample == nil)
                    }
                }
            }

            HStack {
                Text(statusMessage)
                    .font(.callout)
                Spacer()
                Text(_appVersionString)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    /// Maps a MachineControls.ParamID to its backing @State var's
    /// Binding<Double> -- the seam that lets the knob row be generated
    /// from MachineControls.controls(for:mode:) (stage 9) without a
    /// SamplerModel view model (see this file's header note on why that
    /// extraction was skipped): each parameter still lives in its own
    /// @State var here, this switch is just how a descriptor finds it.
    private func _binding(for id: ParamID) -> Binding<Double> {
        switch id {
        case .transpose: return $transposeSemitones
        case .bandwidth: return $sampleRateHz
        case .cutoff: return $filterCutoff
        case .resonance: return $filterResonance
        case .stretch: return $stretchPercent
        case .cycle: return $cycleLength
        case .quality: return $quality
        case .width: return $width
        }
    }

    /// Double-click-to-reset target for one knob -- the current
    /// machine's documented default, same principle for every knob
    /// including bandwidth: as of 2.1, _defaultParams.sampleRateHz IS
    /// the machine's own maxSampleRateHz (never the old 0 sentinel), so
    /// this needs no special case any more -- resetting bandwidth always
    /// lands back at "full."
    private func _defaultValue(for id: ParamID) -> Double? {
        switch id {
        case .transpose: return Double(_defaultParams.transposeSemitones)
        case .bandwidth: return Double(_defaultParams.sampleRateHz)
        case .cutoff: return Double(_defaultParams.filterCutoff01)
        case .resonance: return Double(_defaultParams.filterResonance01)
        case .stretch: return Double(_defaultParams.timeFactorPercent)
        case .cycle: return Double(_defaultParams.cycleLengthSamples)
        case .quality: return Double(_defaultParams.quality)
        case .width: return Double(_defaultParams.width)
        }
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
        do {
            let sample = try audioFileService.load(url: url)
            _adoptAsCurrentSample(sample)
            statusMessage = "Loaded \(url.lastPathComponent)."
            _addToRecentFiles(url)
        } catch {
            statusMessage = "Failed to load: \(error)"
        }
    }

    /// The shared "this is now the source we're working on" reset --
    /// factored out of `_load` for 2.3's "Use as Source" chaining feature
    /// (`_useProcessedAsSource`), which needs the exact same reset against
    /// an in-memory `LoadedSample` that never went through
    /// `audioFileService.load`/a real file URL. Recent-files bookkeeping
    /// is deliberately NOT part of this -- `_load` adds its own real,
    /// on-disk URL after calling this; chaining has no on-disk file yet.
    private func _adoptAsCurrentSample(_ sample: LoadedSample) {
        // A running live session is bound to the previous source's channel
        // count and sample rate -- simplest and safest is to stop it
        // rather than try to reconcile those with whatever loads next.
        // Offline playback (AudioPlaybackController) reconnects itself to
        // a new source's format on the next play(), so stopping it here is
        // just "don't keep the old source sounding".
        isLiveAuditionOn = false
        _stopLiveAudition()
        playback.stop()
        isPlayingOffline = false

        loadedSample = sample
        lastVerifyResult = nil
        processedChannels = nil
        processedWaveformSamples = nil
        renderedSnapshot = nil
        hasUnsavedProcessedAudio = false
        startFrame = 0 // source-specific -- a new source's start point is 0, never inherited
        renderedStartFrame = nil
        playingSource = .none
        playheadFraction = nil

        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let channels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)
        originalWaveformSamples = channels.first ?? []
    }

    /// 2.3 feedback (the S950 chain-restretch trick: stretch, then stretch
    /// the *result* again, repeatably, for longer/gnarlier output) --
    /// deliberately no limit on how many times, per the user's own call
    /// not to model the hardware's "enough already" ceiling.
    ///
    /// Reuses saveProcessed()'s exact LoadedSample construction (interleave
    /// -> PCMConversion.fromFloat -> LoadedSample), just hands the result
    /// to `_adoptAsCurrentSample` instead of a save panel -- in-memory
    /// only, no file write. `channels` is already trimmed to the current
    /// start point (see playProcessed's comment), so chaining also folds
    /// in whatever start-point trim was set, which is the intended
    /// behaviour: the new source IS what was actually processed.
    ///
    /// Undo/redo are cleared, not carried over: the param *values* are
    /// untouched, but every snapshot on either stack describes an edit
    /// against the OLD source -- undoing past this point would silently
    /// re-render old parameter values against audio they were never
    /// designed against.
    private func _useProcessedAsSource() {
        guard let sample = loadedSample, let channels = processedChannels else { return }
        let interleaved = PCMConversion.interleave(channels)
        let rawData = PCMConversion.fromFloat(interleaved, format: sample.format)
        let newSample = LoadedSample(url: sample.url, format: sample.format, rawData: rawData)
        _adoptAsCurrentSample(newSample)
        undoStack.removeAll()
        redoStack.removeAll()
        statusMessage = "Using processed render as the new source -- chain again, or Save Processed to keep this generation on disk."
    }

    private func _addToRecentFiles(_ url: URL) {
        recentFiles.removeAll { $0 == url }
        recentFiles.insert(url, at: 0)
        if recentFiles.count > maxRecentFiles {
            recentFiles.removeLast(recentFiles.count - maxRecentFiles)
        }
        recentFilesStore.save(recentFiles)
    }

    /// 2.3.2 feedback: a way to clear the Recent list. Doesn't touch any
    /// file on disk -- this list is just a shortcut back to files already
    /// opened once, same as `_addToRecentFiles` only ever adds a
    /// reference, never a copy.
    private func _clearRecentFiles() {
        recentFiles = []
        recentFilesStore.save(recentFiles)
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
        guard let provider = providers.first else {
            return false
        }
        // Reject the app's own drag-out export outright -- see
        // UTType.patinaOwnDragExport's comment (ProcessedWavExport.swift)
        // for why letting this fall through to loadFileRepresentation
        // below (which used to be the only guard, and still is one, in
        // _loadDroppedURL) isn't good enough: that check runs AFTER the
        // drop is accepted and materialize/write already happened, too
        // late to avoid the self-drop hang this is actually fixing.
        if provider.hasItemConformingToTypeIdentifier(UTType.patinaOwnDragExport.identifier) {
            return false
        }
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
                DispatchQueue.main.async {
                    _loadDroppedURL(destination)
                }
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

    /// Reads the eleven parameter @State vars into one comparable/
    /// undoable value. The single place both _currentParams() and every
    /// bulk-write path (undo, revert, preset apply) read from or compare
    /// against.
    private func _snapshot() -> ParamSnapshot {
        ParamSnapshot(
            machine: selectedMachine, engine: selectedEngine, mode: selectedMode,
            stretchPercent: stretchPercent, cycleLength: cycleLength,
            quality: quality, width: width, transposeSemitones: transposeSemitones,
            filterCutoff: filterCutoff, filterResonance: filterResonance,
            sampleRateHz: sampleRateHz
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
        processedChannels != nil && (renderedSnapshot != _snapshot() || renderedStartFrame != startFrame)
    }

    /// Converts whichever source is actually playing's OWN [0, 1)
    /// playheadFraction into the shared referenceCount-space fraction
    /// WaveformView draws against (see its own doc comment on
    /// startFraction/playheadFraction for why that space, not
    /// samples.count, is the right one).
    ///
    /// - .original/.live played `_trimmedToStartFrame(original)`, i.e.
    ///   original[startFrame...] -- so frame 0 of ITS OWN fraction is
    ///   really originalWaveformSamples[startFrame], not [0]. Shared
    ///   fraction = (startFrame + own fraction * trimmed length) /
    ///   referenceCount.
    /// - .processed played processedChannels, which was ALREADY
    ///   rendered from a trimmed source (ProcessedRender.render's own
    ///   startFrame) -- so its own fraction 0 really is the trimmed
    ///   render's own frame 0, with no further startFrame offset to add.
    ///   Shared fraction = own fraction * processedCount / referenceCount.
    private var _playheadSharedFraction: Double? {
        guard let playheadFraction else { return nil }
        let referenceCount = max(originalWaveformSamples.count, processedWaveformSamples?.count ?? 0)
        guard referenceCount > 0 else { return nil }

        switch playingSource {
        case .none:
            return nil
        case .original, .live:
            let trimmedLength = max(0, originalWaveformSamples.count - startFrame)
            guard trimmedLength > 0 else { return nil }
            let framesIn = playheadFraction * Double(trimmedLength)
            return (Double(startFrame) + framesIn) / Double(referenceCount)
        case .processed:
            guard let processedCount = processedWaveformSamples?.count, processedCount > 0 else { return nil }
            return (playheadFraction * Double(processedCount)) / Double(referenceCount)
        }
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
        sampleRateHz = defaults.sampleRateHz
        // S950 has no CYCLIC/INTELLIGENT switch at all (Mon1/Pol2
        // instead -- plan section 3.2). Force the picker back to a real
        // state rather than silently ignoring a stale "Intelligent"
        // selection the engine itself would ignore too.
        if StretchProcessor.profile(for: machine).hasModeSwitch == 0 {
            selectedMode = AkzStretchMode_Cyclic
        }
        // Live audition stays running across a machine switch (v2
        // heritage-roster plan, stage 11 fix) -- it used to force-stop
        // for any non-stretch machine, under the same flawed assumption
        // the Preview/Process buttons' old `!_stretchIsSupported` guard
        // made: that a machine with no time-stretch has nothing worth
        // previewing. Filter, transpose and (now) bandwidth all still
        // apply and are all audible live; StretchEngine.cpp's own
        // supportsTimeStretch gate makes sending it stretch params on
        // such a machine harmless regardless.
        _pushLiveParamsIfNeeded()
    }

    /// Assigns all eleven params from a snapshot in one shot -- undo
    /// restore, revert, and preset apply all funnel through this.
    /// Assigning selectedMachine directly here (never through
    /// _selectMachine) is what keeps a bulk write from triggering the
    /// machine-change reset above and clobbering the very values being
    /// restored.
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
        // Clamped into s.machine's own range rather than assigned
        // directly: machine(forStableId:) falls back to S950 for an
        // unrecognised id (a preset saved by a future build naming a
        // machine this one doesn't have), which could otherwise land a
        // rate from that machine's range onto a different machine's
        // knob. The DSP clamps regardless (RateModel.cpp), but the LCD/
        // knob display staying honest is the whole point of 2.1's
        // bandwidth legibility fix.
        let profile = StretchProcessor.profile(for: s.machine)
        sampleRateHz = min(max(s.sampleRateHz, profile.minSampleRateHz), profile.maxSampleRateHz)
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
    /// sampler would be startling) and resets the other ten params to
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
        renderedStartFrame = nil
        hasUnsavedProcessedAudio = false
        statusMessage = "Reverted to \(machineProfile.displayName) defaults."
    }

    // -- recomputing busy light + waveform transport poll --------------------

    /// Was _pollRecomputing -- renamed once it started polling the
    /// playhead too (2.1 feedback: "show playback bar over sample
    /// waveform"). One 20Hz main-thread poll shared by both, same
    /// pattern as the busy-light poll already used (a C++ atomic /
    /// AVAudioPlayerNode's own render-time bookkeeping, neither of which
    /// has anything to subscribe to).
    private func _pollTransport() {
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

        switch playingSource {
        case .none:
            playheadFraction = nil
        case .original, .processed:
            playheadFraction = playback.playheadFraction
        case .live:
            playheadFraction = liveController?.playheadFraction
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
        playingSource = .none
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
        let channels = _trimmedToStartFrame(PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount))

        let controller = LiveAuditionController(channelCount: sample.channelCount, sampleRateHz: sample.sampleRateHz)
        controller.setSource(channels: channels)
        controller.setParams(_currentParams())
        do {
            try controller.start()
            liveController = controller
            playingSource = .live
            statusMessage = "Live audition running -- drag Stretch/Cycle to hear changes."
        } catch {
            statusMessage = "Live audition failed to start: \(error)"
            isLiveAuditionOn = false
        }
    }

    private func _stopLiveAudition() {
        liveController?.stop()
        liveController = nil
        if playingSource == .live { playingSource = .none }
    }

    /// Called from every param-affecting onChange handler. A no-op
    /// unless live audition is actually running, so normal offline use
    /// (Process/Save) pays nothing for this.
    private func _pushLiveParamsIfNeeded() {
        guard isLiveAuditionOn, let controller = liveController else { return }
        controller.setParams(_currentParams())
    }

    /// Sibling of _pushLiveParamsIfNeeded, for the start point (2.1).
    /// Called on SCRUB END, not every drag frame: setSource() kicks a
    /// full background recompute on every channel and resets the
    /// realtime read position, so firing it per drag frame would thrash
    /// the worker threads for no audible benefit -- the marker follows
    /// the cursor live regardless (WaveformView draws it from
    /// `startFrame` directly), and the audio catches up once the drag
    /// ends. Matches AkaizerCore.h's documented "audition is not
    /// expected to be phase-continuous across a change."
    private func _pushLiveSourceIfNeeded() {
        guard isLiveAuditionOn, let controller = liveController, let sample = loadedSample else { return }
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let channels = _trimmedToStartFrame(PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount))
        controller.setSource(channels: channels)
    }

    private func process() {
        guard let sample = loadedSample else { return }

        let snapshot = _snapshot()
        let outputChannels = ProcessedRender.render(sample: sample, params: snapshot.params, startFrame: startFrame)

        processedChannels = outputChannels
        processedWaveformSamples = outputChannels.first
        renderedSnapshot = snapshot
        renderedStartFrame = startFrame
        hasUnsavedProcessedAudio = true
        let outFrames = outputChannels.first?.count ?? 0
        statusMessage = "Processed: \(sample.frameCount) → \(outFrames) frames (\(String(format: "%.2f", Double(outFrames) / sample.sampleRateHz))s)."
    }

    /// Slices `channels` (full-length, one array per channel) at
    /// startFrame -- the one place every playback/live-audition path
    /// trims to the current start point. Clamped, not trusted: past the
    /// end clamps to empty per channel rather than crashing.
    private func _trimmedToStartFrame(_ channels: [[Float]]) -> [[Float]] {
        channels.map { channel in
            let clamped = min(max(0, startFrame), channel.count)
            return Array(channel[clamped...])
        }
    }

    private func playOriginal() {
        guard let sample = loadedSample else { return }
        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let channels = _trimmedToStartFrame(PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount))
        do {
            try playback.play(channels: channels, sampleRateHz: sample.sampleRateHz)
            isPlayingOffline = true
            playingSource = .original
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
        // Trimmed to the SAME start point processedChannels was
        // rendered from (2.1), so the loudness reference stays honest --
        // `channels` itself is NOT trimmed again here: it's already the
        // trimmed render (see ProcessedRender.render's startFrame), and
        // double-trimming it would cut the start point twice over.
        let originalInterleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let originalChannels = _trimmedToStartFrame(PCMConversion.deinterleave(originalInterleaved, channelCount: sample.channelCount))
        let referenceRMS = PCMConversion.rms(originalChannels)
        let gain = PCMConversion.matchedGain(channels, toMatchRMS: referenceRMS)
        let matchedChannels = PCMConversion.applyGain(channels, gain: gain)

        do {
            try playback.play(channels: matchedChannels, sampleRateHz: sample.sampleRateHz)
            isPlayingOffline = true
            playingSource = .processed
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
            hasUnsavedProcessedAudio = false
        }
    }

    /// v2 heritage-roster plan, stage 11. Renders fresh from
    /// _currentParams() -- the exact same params live audition is
    /// already playing, by construction (both paths have always read
    /// from this one function; see its own doc comment) -- so what
    /// this saves is provably what's audible right now, not the last
    /// offline Process() result Save Processed uses. Live audition
    /// itself is never touched: rendering happens on a detached Task,
    /// same pattern as ProcessedWavExport's drag-out export, and the
    /// result is adopted as the new processedChannels (so Play
    /// Processed/Save Processed/the waveform overlay all pick it up
    /// too) before reusing saveProcessed()'s own save-panel flow.
    private func _saveWhatImHearing() {
        guard let sample = loadedSample else { return }
        let snapshot = _snapshot()
        let params = snapshot.params
        let capturedStartFrame = startFrame
        Task {
            let channels = await Task.detached(priority: .userInitiated) {
                ProcessedRender.render(sample: sample, params: params, startFrame: capturedStartFrame)
            }.value
            processedChannels = channels
            processedWaveformSamples = channels.first
            renderedSnapshot = snapshot
            renderedStartFrame = capturedStartFrame
            hasUnsavedProcessedAudio = true
            saveProcessed() // clears hasUnsavedProcessedAudio back to false on a completed save
        }
    }

    /// Builds the value behind WaveformView's .draggable() -- see
    /// ProcessedWavExport.swift's header for the full rationale. Cheap by
    /// design: hands over the current snapshot/cached-channels rather
    /// than doing any rendering here, so this can be called fresh on
    /// every body evaluation without cost.
    private func _dragExport(for sample: LoadedSample) -> ProcessedWavExport {
        let capturedStartFrame = startFrame
        return ProcessedWavExport(
            source: sample,
            snapshot: _snapshot(),
            cachedChannels: _renderIsStale ? nil : processedChannels,
            startFrame: capturedStartFrame,
            fileName: sample.url.deletingPathExtension().lastPathComponent + "-stretched.wav",
            onRendered: { channels, snapshot in
                // ProcessedWavExport invokes this via `DispatchQueue.main.
                // async` (2.3.2 -- was `await MainActor.run`, changed
                // because that specific await was where an abandoned/
                // never-dropped drag would hang forever; see that file's
                // comment), so it's still genuinely always on the main
                // actor -- assumeIsolated tells the compiler that rather
                // than hopping again, since the @Sendable closure type
                // (required to cross into the async export path) can't
                // itself express that guarantee.
                MainActor.assumeIsolated {
                    processedChannels = channels
                    processedWaveformSamples = channels.first
                    renderedSnapshot = snapshot
                    renderedStartFrame = capturedStartFrame
                    hasUnsavedProcessedAudio = true
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
