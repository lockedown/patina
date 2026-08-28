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
        .onAppear {
            presets = presetStore.load()
            playback.onFinished = { isPlayingOffline = false }
            _autoloadIfRequested()
        }
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
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Button("Open WAV/AIFF…", action: openFile)
                Button("Save Unchanged Copy…", action: saveCopy)
                    .disabled(loadedSample == nil)
                Button("Verify Round Trip", action: verifyRoundTrip)
                    .disabled(loadedSample == nil)
            }

            LCDReadoutView(rows: _lcdRows)

            if loadedSample != nil {
                WaveformView(samples: originalWaveformSamples, overlaySamples: processedWaveformSamples)
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
                    Picker("Machine", selection: $selectedMachine) {
                        ForEach(StretchProcessor.allMachines, id: \.rawValue) { machine in
                            Text(StretchProcessor.profile(for: machine).displayName).tag(machine)
                        }
                    }
                    .onChange(of: selectedMachine) { _, newMachine in
                        let defaults = StretchProcessor.defaultParams(machine: newMachine)
                        stretchPercent = Double(defaults.timeFactorPercent)
                        cycleLength = Double(defaults.cycleLengthSamples)
                        quality = Double(defaults.quality)
                        width = Double(defaults.width)
                        transposeSemitones = Double(defaults.transposeSemitones)
                        filterCutoff = Double(defaults.filterCutoff01)
                        filterResonance = Double(defaults.filterResonance01)
                        // S950 has no CYCLIC/INTELLIGENT switch at all
                        // (Mon1/Pol2 instead -- plan section 3.2). Force
                        // the picker back to a real state rather than
                        // silently ignoring a stale "Intelligent"
                        // selection the engine itself would ignore too.
                        if StretchProcessor.profile(for: newMachine).hasModeSwitch == 0 {
                            selectedMode = AkzStretchMode_Cyclic
                        }
                        if StretchProcessor.profile(for: newMachine).supportsTimeStretch == 0 {
                            isLiveAuditionOn = false // triggers its own onChange -> _stopLiveAudition()
                        } else {
                            _pushLiveParamsIfNeeded()
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

                    Picker("Engine", selection: $selectedEngine) {
                        Text("Classic").tag(AkzEngine_Classic)
                        Text("Revised").tag(AkzEngine_Revised)
                    }
                    .pickerStyle(.segmented)

                    // Transpose is a basic sampler feature every one of
                    // these machines has (unlike time-stretch, which the
                    // S900 lacks -- plan section 3.1), so it's shown
                    // unconditionally rather than gated on
                    // _stretchIsSupported. Varispeed: pitch and duration
                    // move together, matching the real hardware -- see
                    // Interpolator.h.
                    _sliderRow(
                        "Transpose", value: $transposeSemitones,
                        range: -36...36,
                        format: "%.0f st"
                    )

                    // Filter (build order stage 6) applies regardless of
                    // whether the machine supports time-stretch -- every
                    // one of these machines has SOME VCF (plan section
                    // 3.1), so cutoff is shown unconditionally. Resonance
                    // only does anything on S2000/S3000/S3200 (plan
                    // section 3.2 item 2 -- the S2000 correction) so it's
                    // hidden rather than shown-but-inert elsewhere.
                    _sliderRow(
                        "Cutoff", value: $filterCutoff,
                        range: 0...1,
                        format: "%.2f"
                    )
                    if _filterHasResonance {
                        _sliderRow(
                            "Resonance", value: $filterResonance,
                            range: 0...1,
                            format: "%.2f"
                        )
                    }

                    if _stretchIsSupported {
                        _sliderRow(
                            "Stretch", value: $stretchPercent,
                            range: 25...max(25.0, Double(machineProfile.maxStretchPercent)),
                            format: "%.0f%%"
                        )

                        if _hasModeSwitch {
                            Picker("Mode", selection: $selectedMode) {
                                Text("Cyclic").tag(AkzStretchMode_Cyclic)
                                Text("Intelligent").tag(AkzStretchMode_Intelligent)
                            }
                            .pickerStyle(.segmented)
                        }

                        // Cycle length only means anything in CYCLIC;
                        // quality/width only in INTELLIGENT (plan "2.2",
                        // and each field's own doc comment in
                        // AkaizerCore.h) -- shown accordingly rather than
                        // all-visible-but-some-inert.
                        if _isIntelligentMode {
                            _sliderRow(
                                "Quality", value: $quality,
                                range: 0...99,
                                format: "%.0f"
                            )
                            _sliderRow(
                                "Width", value: $width,
                                range: 0...99,
                                format: "%.0f"
                            )
                        } else {
                            _sliderRow(
                                "Cycle", value: $cycleLength,
                                range: 20...2000,
                                format: "%.0f smp"
                            )
                        }
                    } else {
                        // S900 predates the S950's time-stretch feature
                        // entirely (plan section 3.1) -- maxStretchPercent
                        // is 0 for it, which would make 25...0 an invalid
                        // range and crash the Slider. Rather than lean on
                        // the `max(25.0, ...)` clamp above alone as the
                        // only thing standing between this and a crash,
                        // don't offer stretch controls for a machine that
                        // structurally doesn't have the feature.
                        Text("\(machineProfile.displayName) has no time-stretch capability (added in the S950).")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }

                    Toggle("Live Audition", isOn: $isLiveAuditionOn)
                        .disabled(loadedSample == nil || !_stretchIsSupported)
                        .onChange(of: isLiveAuditionOn) { _, on in
                            if on { _startLiveAudition() } else { _stopLiveAudition() }
                        }

                    HStack {
                        Button("Process", action: process)
                            .disabled(loadedSample == nil || !_stretchIsSupported)
                        // A/B: "compares processed against dry original
                        // at matched loudness" (plan) -- Play Processed
                        // scales its output to match Play Original's RMS
                        // (see playProcessed), and A/B are keyboard-
                        // toggled per the plan's own wording.
                        Button("Play Original", action: playOriginal)
                            .disabled(loadedSample == nil || isLiveAuditionOn)
                            .keyboardShortcut("a", modifiers: [])
                        Button("Play Processed", action: playProcessed)
                            .disabled(processedChannels == nil || isLiveAuditionOn)
                            .keyboardShortcut("b", modifiers: [])
                        // One button that stops whichever audio path is
                        // currently active -- offline playback, live
                        // audition, or (in principle, though the rest of
                        // this UI already prevents it) both.
                        Button("Stop", action: _stopAllAudio)
                            .disabled(!isPlayingOffline && !isLiveAuditionOn)
                            .keyboardShortcut(".", modifiers: .command)
                        Button("Save Processed…", action: saveProcessed)
                            .disabled(processedChannels == nil)
                        Button("Save Preset…", action: _promptSavePreset)
                            .disabled(loadedSample == nil)
                    }
                }
            }

            Text(statusMessage)
                .font(.callout)

            Spacer()
        }
        .padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    private func _sliderRow(_ label: String, value: Binding<Double>, range: ClosedRange<Double>, format: String) -> some View {
        HStack {
            Text(label)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .frame(width: 84, alignment: .leading)
            Slider(value: value, in: range)
            Text(String(format: format, value.wrappedValue))
                .font(.system(.body, design: .monospaced))
                .frame(width: 70, alignment: .trailing)
        }
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

    /// Builds params from the current picker/slider state -- the single
    /// place both the offline Process button and live audition read from,
    /// so they can never drift apart.
    private func _currentParams() -> AkzStretchParams {
        var params = StretchProcessor.defaultParams(machine: selectedMachine)
        params.engine = selectedEngine
        params.mode = selectedMode
        params.timeFactorPercent = Float(stretchPercent)
        params.cycleLengthSamples = Int32(cycleLength)
        params.quality = Int32(quality)
        params.width = Int32(width)
        params.transposeSemitones = Float(transposeSemitones)
        params.filterCutoff01 = Float(filterCutoff)
        params.filterResonance01 = Float(filterResonance)
        return params
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

    /// Applies a preset's params to every slider/picker. Sets
    /// selectedMachine FIRST, deliberately: that field's own onChange
    /// resets every other field to the new machine's defaults, so
    /// anything set after it here (the preset's actual values) is what
    /// sticks -- setting it last would let the reset clobber the preset.
    private func _applyPreset(_ preset: AkaizerPreset) {
        let params = preset.params
        selectedMachine = params.machine
        selectedEngine = params.engine
        selectedMode = params.mode
        stretchPercent = Double(params.timeFactorPercent)
        cycleLength = Double(params.cycleLengthSamples)
        quality = Double(params.quality)
        width = Double(params.width)
        transposeSemitones = Double(params.transposeSemitones)
        filterCutoff = Double(params.filterCutoff01)
        filterResonance = Double(params.filterResonance01)
        _pushLiveParamsIfNeeded()
        statusMessage = "Applied preset \"\(preset.name)\"."
    }

    private func _deletePreset(_ preset: AkaizerPreset) {
        presets.removeAll { $0.id == preset.id }
        presetStore.save(presets)
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

        let params = _currentParams()

        let interleaved = PCMConversion.toFloat(sample.rawData, format: sample.format)
        let inputChannels = PCMConversion.deinterleave(interleaved, channelCount: sample.channelCount)

        // Each channel is stretched independently with identical
        // parameters -- the real hardware has no notion of stereo
        // linkage inside the stretch algorithm itself (plan section 2
        // describes it purely per-sample-stream).
        var outputChannels: [[Float]] = []
        for channel in inputChannels {
            let processor = StretchProcessor(sampleRateHz: sample.sampleRateHz)
            processor.setParams(params)
            processor.setSource(channel)
            outputChannels.append(processor.renderAll())
        }

        processedChannels = outputChannels
        processedWaveformSamples = outputChannels.first
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
