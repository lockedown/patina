# Akaizer S

A native Apple Silicon app that recreates the time-stretch and converter/filter character of the Akai S900/S950/S1000/S2000/S3000/S3200 samplers — the "cyclic" stretch behind decades of jungle, hip-hop and breakcore production. Built to succeed [Akaizer](https://the-akaizer-project.blogspot.com/) (Windows-only, closed-source, now end-of-life) with real-time audition and per-machine converter modelling it never had.

This is a clean-room implementation: no Akaizer code, no ROM disassembly. The DSP is built from published Akai owner's/service manuals and validated by ear against real hardware behaviour. See `/Users/locked/.claude/plans/snuggly-stargazing-deer.md` for the full research and design rationale.

## Status

**All 9 build-order stages complete.** See "Known limitations" below for what that does and doesn't mean.

**Distribution:** a real, iconed, unsigned `.app` bundle — see "Distribution" below. `swift build`/`swift run` remain the normal dev workflow; the bundle is for handing the app to someone else without them needing this repo.

1. ✅ C++ DSP core skeleton + C API + test harness
2. ✅ File I/O and null signal path — WAV/AIFF load → save unchanged, verified bit-identical
3. ✅ CLASSIC + REVISED stretch, offline — wired into the app (machine picker, stretch/cycle sliders, Process/Play/Save), with playback via AVAudioEngine, and artifact-verified: `Tests/CoreTests/ArtifactTests.cpp` proves the documented metallic-ringing (short cycle) and tremolo (long cycle) mechanisms actually occur, by measuring output autocorrelation at the cycle lag rather than trusting the implementation to match the description
4. ✅ Real-time audition — a "Live Audition" toggle drives a render-thread-safe `AkzRealtimePlayer` (background recompute thread, published to an `AVAudioSourceNode` render callback via `std::atomic_load`/`store` on `shared_ptr`, never allocating on the render thread). Sliders/pickers push new params live while it runs. Offline Process/Play/Save is unchanged, a separate path
5. ✅ Transpose + per-machine interpolators — `Interpolator.{h,cpp}` implements varispeed (pitch and duration move together, matching the real hardware) with two interpolation kinds: zero-order hold for S2000/S3000/S3200 (MAME-confirmed) and linear for S900/S950/S1000. Applied after the stretch step, per the signal chain in the plan. A Transpose slider (±36 semitones, shown unconditionally — every machine here can transpose, unlike time-stretch) sits above Stretch/Cycle in the UI
6. ✅ Converter + filter models — `ConverterModel.{h,cpp}` re-quantises the source to the machine's native bit depth fresh on every recompute (12-bit S900/S950, 16/18-bit others; no dither, no companding). `FilterModel.{h,cpp}` applies the machine-appropriate VCF last in the chain: the exact MAME-derived Chamberlin SVF difference equation (resonant, cutoff fixed) for S2000/S3000/S3200 — S3200 running two in series for 24 dB/oct — and a cascaded one-pole lowpass (no resonance, cutoff tracks transpose ratio) for S900/S950/S1000. Cutoff/Resonance sliders added (Resonance hidden on machines without it). Deliberately not modelled yet, flagged in `ConverterModel.h`: S900/S950's variable sample rate/bandwidth, and the S2000/S3000 DAC's rising low-level distortion — both real, both need more design work than this stage budgeted for
7. ✅ INTELLIGENT mode + per-machine stretch variants — `StretchEngine::_synthesizeIntelligent` implements SOLA (synchronous overlap-add): instead of splicing at a fixed cycle length, each frame's exact read offset is chosen by cross-correlating against the tail already written, searching within a `quality`-controlled range around the nominal position — "intelligently varies the interpolation rate according to the sample content," per the manual. `width` controls the crossfade region, which determines the synthesis hop. A shared `IntelligentPlan` helper derives every parameter once so the synthesis and the length-only query can't drift apart. S950's lack of a mode switch is enforced in the engine itself (params.mode is ignored, not just conventionally unused) as well as in the UI. Mode/Quality/Width controls added, gated per the manual (Cycle only in CYCLIC, Quality/Width only in INTELLIGENT)
8. ✅ UI redesign — sidebar (Recent files, a jump list, not the batch queue the plan ruled out) plus `LCDReadoutView`, the one deliberately retro element: a fixed-palette phosphor-green monospace panel modelled on the real S3200XL time-stretch screen's own lowercase `label: value` convention from the research, not a generic "green terminal" look. Everything else stays standard adaptive Mac system colours/controls, per the plan's "modern base, retro accents" direction — the retro moment is scoped to one panel, not a whole-app theme
9. ✅ Presets, A/B, waveform display — `PresetStore.swift` persists named parameter sets ("Jungle S950," "Dusty S1000," the plan's own examples) as one JSON file in Application Support; a sidebar Presets section lists, applies, and deletes them. `WaveformView.swift` overlays the original (dim) and processed (bright) traces in the same phosphor-green palette as the LCD readout — display only, not scrubable (see that file's header for why: real seek needs transport plumbing `AVAudioPlayerNode` doesn't offer for free). Play Original/Play Processed are keyboard-toggled (`a`/`b`) and loudness-matched — Play Processed's RMS is scaled to match Play Original's, so a level difference never gets mistaken for the stretch effect itself

**Gotchas fixed along the way:**
- A bare SwiftUI app run via `swift run`/the raw binary (no `.xcodeproj`, no proper `.app` bundle) launches with `NSApplication.ActivationPolicy.prohibited` by default — no window, no Dock icon, silently. Confirmed by querying `NSWorkspace.runningApplications` when the app appeared to hang with nothing on screen. Fixed with an explicit `NSApplicationDelegate` in `AkaizerSApp.swift` that sets `.regular` and activates on launch.
- Selecting **S900** in the Machine picker crashed the app: S900 has no time-stretch capability (added in the S950), so `maxStretchPercent` is `0.0` for it, making the Stretch slider's `25...0.0` range invalid — a guaranteed, 100%-reproducible crash on a completely normal user action. Found by actually driving the GUI (`osascript`/`System Events`), not by unit tests. Fixed two ways: the Stretch/Cycle controls are replaced with an explanatory message and the Live Audition/Process controls disabled whenever `machineProfile.supportsTimeStretch == 0`, and the range construction itself is clamped (`max(25.0, ...)`) as defense in depth rather than relying on the UI guard alone.
- `ConverterModel::quantize()` produced 2^bits + 1 levels, not 2^bits: clamping the scaled *value* to `≤ 1.0` after rounding lets an input that rounds to exactly +1.0 sail straight through (`1.0 > 1.0` is false), adding an extra level at the top. Caught by a test asserting the exact level count. Fixed by clamping the integer *index* to the standard asymmetric signed-PCM range (`[-levels/2, levels/2 - 1]`) before scaling, which is exactly right by construction rather than by a boundary check that can be dodged.
- **The resonant `ChamberlinSVF` diverged to ±infinity at its own default settings** (`filterCutoff01 = 1.0`, `filterResonance01 = 0.0` — fully open, no resonance boost). Stage 6's stability comment already flagged that the naive Chamberlin SVF gets "inaccurate/unstable as k approaches 2," and clamped `k` to 1.9 believing that was a safe margin below the boundary — it wasn't. An empirical sweep (all 16 resonance codes, 200k samples) found the real boundary at `k ≈ 1.23`, and at the worst-case damping (least resonance — the default) instability set in within a few hundred samples. Caught by stage 7's own longer-running INTELLIGENT tests (`FilterModelTests.cpp`'s shorter windows and non-default cutoffs happened not to exercise this at all). Fixed by clamping `k` to 1.1 (confirmed stable across every resonance code over 200k samples) and adding a hard per-sample state clamp as a backstop regardless — no input should ever be able to make this filter emit non-finite values into the rest of the pipeline. Real consequence: this SVF can't reach the full 20 Hz..Nyquist range its cutoff control implies; it caps out around 8 kHz at 44.1 kHz.

**Bug fixes (post-9-stage, real-use bugfix pass):**
- **CYCLIC was not an identity at 100% time factor — it audibly buzzed with nothing asked of it.** The crossfade in `_synthesizeCyclicBlocks` (`StretchEngine.cpp`) blended each block's tail against material read `overlap` samples into the *future* of the natural continuation, rather than the same output timebase — comb-filtering the last quarter of every block even when adjacent blocks were already contiguous and had no splice to hide at all. Reported as "the S950 sound seems distorted" — the initial hypothesis, that S950's 6-pole filter cascade over-attenuates versus S1000's 3-pole, was investigated and **measured, then refuted** (worst-case deviation ~4.7 dB at Nyquist, a tilt, not distortion): S950 was simply the only stretch-capable machine whose UI can't select INTELLIGENT (`hasModeSwitch == 0`), so it was the only place the underlying bug — present in CYCLIC on every machine — was ever heard. Fixed by reading the crossfade's second leg on the same output timebase as the first; verified to reduce artifact energy at 150–200% stretch by ~26 dB and make 100% a true identity (`Tests/CoreTests/StretchEngineTests.cpp`).
- **Live audition restarted playback on every slider movement, including filter/resonance-only changes.** `RealtimeStretchPlayer`'s worker did a full re-render and unconditionally reset the read position on every `setParams()`, with no distinction between a filter-only change (which can't affect buffer length) and a stretch-affecting one. Fixed with `StretchEngine::reapplyFilterOnly()` (redoes just the cached pre-filter buffer) plus `paramsDifferOnlyInFilter()` classification in the worker loop: a filter-only change now leaves the read position untouched, and a stretch-affecting change remaps it proportionally into the new buffer instead of snapping to 0 (`Tests/CoreTests/RealtimeStretchPlayerTests.cpp`).
- **No Stop control, and no signal when playback finished on its own.** `AudioPlaybackController.stop()` existed but was never exposed in the UI, and `scheduleBuffer` had no completion callback. Added a generation-counted completion callback (`onFinished`) and a Stop button wired to it.
- **Live Audition and offline Play Original/Processed could play simultaneously.** Two independent `AVAudioEngine`s (`AudioPlaybackController`, `LiveAuditionController`), and `_startLiveAudition()`/`_load()` never stopped the other one. Fixed by stopping offline playback whenever live audition starts or a new file loads.
- **Regression, caught immediately after the fix above shipped: dragging the Cycle slider during live audition on a stereo file made it sound "stereo width"/comb-y.** The read-position fix for the restart bug remapped position proportionally (`oldPos/oldLen * newLen`) on stretch-affecting changes instead of resetting to 0. `LiveAuditionController` runs one independent `RealtimeStretchPlayer` per channel, each with its own background worker — `oldPos` is a live, continuously-advancing value, and when two channels' workers process the same change at different wall-clock moments (ordinary thread-scheduling skew), each reads a *different* `oldPos` and remaps to a genuinely different, then permanently desynchronised, position. A mono test file during the original fix's verification couldn't have shown this — there's only one channel to desync against. Reverted to resetting to 0 on stretch-affecting changes (a fixed point every channel's worker converges on identically regardless of timing); the filter-only cheap path is unaffected, since it never touches the read position at all. New tests: a single-instance test confirming the reset, and a two-instance test that directly staggers two independent players through the same change and asserts they stay sample-identical.
- **Regression of the regression above: resetting to 0 on stretch-affecting changes stopped the two channels ever staying *permanently* desynchronised, but not from *transiently* diverging while the change was in flight — reported as the same "artificial stereo width" on every stretch-affecting knob (not just Cycle), still present after the fix above.** Each channel's `RealtimeStretchPlayer` published its own re-render and reset its own read position the instant its own worker finished, with zero regard for whether a sibling channel's worker (independent thread, independent completion time) had finished the SAME change yet — so for however long that gap lasted, one channel played the new render from 0 while the other kept playing the old one, which is genuinely different audio on each channel simultaneously. Rotary-knob dragging (finer-grained than the sliders it replaced) re-triggers this gap almost continuously through a drag, which is why it now reads as present on every knob rather than an occasional Cycle-specific glitch. Fixed by splitting publish from commit: a stretch-affecting re-render now lands in a `_pendingPublish` slot (`RealtimeStretchPlayer::commitPending()`/`hasPendingCommit()`) instead of going live immediately, and `LiveAuditionController`'s render callback only calls `commitPending()` on every channel once *all* of them report a pending commit — swapping every channel's buffer and resetting every channel's read position on the exact same audio frame, regardless of which worker actually finished first. The filter-only cheap path is unaffected (still publishes immediately; never needed the gate). New tests: a single-instance test locking in the deferred-publish contract itself (not visible to `pull()` until `commitPending()`), and a two-instance test that stages one channel's worker finishing before the other's and asserts they stay sample-identical throughout — not just once the dust settles, but during the gap itself, which the previous fix's test never actually exercised.

**User-feedback pass (first round of real use, post-9-stage):** seven requests, one of which turned out to already exist —
- **"Preview" button, promoted.** The "test before committing" ask was already built: the Live Audition toggle previews the fully processed result live. It just wasn't found, tucked below the knobs as a plain `Toggle`. Replaced with a prominent play/stop-style `Button` next to the knobs (same `_startLiveAudition`/`_stopLiveAudition` underneath, unchanged), plus a "recomputing…" busy light — which needed new C++: `akz_realtime_player_is_ready` turned out unable to drive it (it latches true forever after the first publish and can never report a *later* re-render), so `RealtimeStretchPlayer` gained a genuinely-toggling `_recomputing` atomic instead (`akz_realtime_player_is_recomputing`), set for the duration of each worker-thread render and polled by `ContentView` with hysteresis (150ms delay before showing, 250ms minimum once shown) so it doesn't strobe during a fast run of knob-drag re-renders.
- **Undo, parameters only.** `ParamSnapshot.swift` mirrors the ten stretch/filter parameters (`AkzStretchParams` itself has no Swift `Equatable` to compare against) and backs a plain `@State` undo/redo stack in `ContentView`, coalesced per-interaction via a new `onEditingChanged` closure on `RotaryKnobView` (mirrors SwiftUI's own `Slider`) so one knob drag is one undo step, not hundreds. Wired to a real Edit menu (`AkaizerSApp.swift`'s first `.commands` block) via `focusedSceneValue`, since `ContentView` has no view model a Scene-level menu could otherwise reach.
- **Revert to original.** The app was already non-destructive (`process()` re-decodes from `loadedSample.rawData`, never mutates it), so this just resets the nine non-machine params to the current machine's defaults and discards the render — one undoable step.
- **Double-click a knob to type a value, ⌥-click to reset.** Double-click was already reset-to-default; moved reset to Option-click (backed by a context-menu item so it stays discoverable) and gave double-click to a new inline `TextField` (`KnobCell.swift`), seeded with a bare number rather than the display string so parsing never has to know about any of the seven `%`/`st`/`smp` suffix formats.
- **Shift-drag a knob for fine adjustment.** 5× less sensitive. The one real design problem was the modifier changing mid-drag without the value jumping — solved by re-anchoring (normalized value, raw translation) at the moment Shift is pressed or released, rather than rescaling the whole drag retroactively.
- **Drag a file into the window to load it.** A window-wide `.onDrop` (not `.dropDestination(for: URL.self)`, which has a documented history of failing to decode Finder's own file-URL payload) routes to the existing `_load(url:)` funnel.
- **Drag the waveform out to export a .wav.** `.draggable`/`Transferable` (`ProcessedWavExport.swift`) — a file promise, so a cold or stale render doesn't stall the drag itself, only the drop. Renders fresh if needed (a drag-out of a stale state doubles as a Process); always exports `.wav` regardless of source container, unlike the Save panel's format-follows-source (safe: everything already normalises to little-endian internally). The per-channel render loop was extracted out of `process()` into `ProcessedRender.swift` so both share one implementation.

## Known limitations

Everything below is a deliberate scope decision flagged at the time, not an oversight discovered later — collected here so the state of the project is legible in one place rather than scattered across code comments.

- **S900/S950's variable sample rate/bandwidth control is not modelled.** The converter only quantises bit depth; it doesn't reduce/reconstruct sample rate the way the real "audio bandwidth" knob does (`fs = bandwidth × 2.5`). Real, audible character; needs its own UI control and design pass (`ConverterModel.h`).
- **The S2000/S3000 DAC's rising low-level distortion isn't modelled.** The real PCM69AP hybrid DAC's THD+N rises sharply into the decay tail (`ConverterModel.h`); reproducing that needs a verified transfer curve this project didn't have.
- **The resonant SVF can't reach its own advertised range.** Capped at ~8 kHz cutoff at 44.1kHz sample rate (the stability fix above) rather than the full 20 Hz..Nyquist the control implies.
- **The S1000's interpolator order is an assumption, not a citation.** Akai's own manual only states "24-bit algorithm, custom VLSI" (arithmetic precision, not filter order); this project assumes linear interpolation pending a by-ear revision (`Interpolator.h`).
- **The waveform display is not scrubable.** Visual only — no click-to-seek, no playhead (`WaveformView.swift`).
- **8-bit AIFF would decode with the wrong signedness.** `PCMConversion.swift` decodes 8-bit as unsigned (the WAV convention); AIFF's 8-bit is actually signed. Not fixed because it hasn't come up — real sample libraries are practically always 16-bit.
- **INTELLIGENT mode's quality→search-range and width→crossfade mappings are this project's own design**, not derived from the hardware (no manual states the actual numeric ranges — plan section 2.2). The algorithm class (SOLA/cross-correlation search) is manual-confirmed; the specific curve mapping the 0–99 controls to sample counts is not. Measured too narrow at low `quality` (±33 samples against a 1323-sample frame at `quality=10`, not enough to phase-align even a 440 Hz tone) — a re-derive is in the backlog.
- **S3000/S3200 clip at fully-open cutoff.** The resonant SVF's `k ≤ 1.1` stability clamp (see the gotcha above) leaves a resonant peak around ~8.2 kHz; a loud signal through it plus `PCMConversion.matchedGain`'s A/B loudness match can exceed 1.0 and hard-clip in `fromFloat`. Needs a passband-gain compensation at the filter, not a downstream limiter.
- **Save Processed saves the last offline `Process()` result, not what live audition is currently playing.** There's no "save what I'm hearing right now" path — the drag-out export (user-feedback pass, above) auto-renders a stale/missing result via the same offline `Process()` path, it doesn't capture live audition's output either.
- **Recent files are session-only**, not persisted (unlike presets, which already use `PresetStore`).

## Project Structure

```
akaizer/
├── CMakeLists.txt               Standalone build for the C++ core + tests (no Xcode needed)
├── Package.swift                Builds the whole app (Core + Audio + App); Xcode can open this directly
├── Sources/
│   ├── Core/                    C++ DSP engine — no Apple frameworks, portable
│   │   ├── include/
│   │   │   └── AkaizerCore.h    Public C API — the only header outside code should include
│   │   ├── MachineProfile.{h,cpp}   Per-machine constants, cited to source manuals
│   │   ├── StretchEngine.{h,cpp}    CLASSIC/REVISED cyclic time-stretch, synchronous (offline use)
│   │   ├── RealtimeStretchPlayer.{h,cpp}  Render-thread-safe wrapper for live audition (background thread + shared_ptr publish)
│   │   ├── Interpolator.{h,cpp}     Per-machine transpose/varispeed — zero-order hold vs linear
│   │   ├── ConverterModel.{h,cpp}   Bit-depth quantisation, no dither
│   │   └── FilterModel.{h,cpp}      Per-machine VCF — resonant Chamberlin SVF vs cascaded one-pole lowpass
│   ├── App/                     SwiftUI views, app lifecycle
│   │   ├── AkaizerSApp.swift        Entry point + the activation-policy fix (see Status) + the Edit menu's Undo/Redo commands
│   │   ├── EditCommands.swift       focusedSceneValue bridge from ContentView's @State undo stack to the Edit menu
│   │   ├── LCDReadoutView.swift     The one retro accent -- fixed-palette phosphor-green readout
│   │   ├── WaveformView.swift       Original/processed overlay, same palette as the LCD -- display only, not scrubable; drag-out export attaches at its call site, not inside it
│   │   ├── KnobCell.swift           One rack-panel knob (label/RotaryKnobView/value) -- owns the double-click-to-type TextField state _knobCell (a function) couldn't hold
│   │   ├── RotaryKnobView.swift     The knob control -- drag-to-adjust (Shift for fine), double-click to type, ⌥-click to reset, onEditingChanged for undo coalescing
│   │   └── ProcessedWavExport.swift Transferable behind the waveform's .draggable() -- drag-out .wav export, auto-rendering a stale/missing result first
│   └── Audio/                   WAV/AIFF codecs, Swift/C++ bridging
│       ├── WavCodec.swift           Hand-rolled RIFF/WAVE reader/writer
│       ├── AiffCodec.swift          Hand-rolled FORM/AIFF reader/writer (incl. 80-bit extended float sample rate)
│       ├── PCMConversion.swift      Raw PCM bytes ↔ Float32, for the DSP core and playback; RMS/gain matching for A/B
│       ├── StretchBridge.swift      Swift wrapper around the C API (StretchProcessor + RealtimePlayer)
│       ├── AudioPlaybackController.swift  AVAudioEngine playback of Float32 buffers (offline Play)
│       ├── LiveAuditionController.swift   AVAudioSourceNode-driven live audition, one RealtimePlayer per channel
│       ├── PresetStore.swift        Named parameter sets, persisted as one JSON file in Application Support
│       ├── ParamSnapshot.swift      Swift-Equatable mirror of AkzStretchParams' ten fields -- backs undo, render-staleness, and revert alike
│       └── ProcessedRender.swift    The decode → per-channel StretchProcessor → render loop, extracted out of process() so the drag-out export shares it
├── Resources/                    Checked-in app bundle assets (not source code)
│   ├── Info.plist                    Bundle metadata -- see "Distribution"
│   └── AppIcon.icns                  Generated once from a stretching-waveform glyph, same palette as the LCD
├── Scripts/
│   └── build_app_bundle.sh       Assembles dist/Akaizer S.app from a release build -- see "Distribution"
└── Tests/
    ├── CoreTests/                Zero-dependency C++ unit tests (TestFramework.h — no GoogleTest/Catch2), incl. ArtifactTests.cpp, RealtimeStretchPlayerTests.cpp (incl. a concurrency stress test, TSan-clean), InterpolatorTests.cpp, ConverterModelTests.cpp, FilterModelTests.cpp and IntelligentModeTests.cpp
    ├── AudioTests/               XCTest — WAV/AIFF bit-exact round trip + full decode→stretch→encode→save chain, incl. transpose, PresetStoreTests.swift, PCMConversionLoudnessTests.swift, ParamSnapshotTests.swift and ProcessedRenderTests.swift
    └── Fixtures/                 Reference audio for round-trip and (later) by-ear fidelity checks
```

## Tech Stack

- **C++17** — DSP core, portable, no Apple framework dependencies
- **Swift + SwiftUI** — app shell
- **Hand-rolled WAV/AIFF codecs** — see "Why not AVAudioFile" below
- **Accelerate** — vectorised DSP (planned, stage 4+)
- **CMake 3.14+** — builds and tests the C++ core standalone, independent of Xcode
- **Swift Package Manager** — builds the whole app; also how Xcode opens this project (no `.xcodeproj` maintained)
- Zero third-party dependencies, by design

### Why not AVAudioFile

Tried first, dropped deliberately. On this machine's SDK (macOS 26.5 SDK / Xcode 26.6), `AVAudioFile` has two reproducible bugs that break the bit-exactness this app depends on: reading 16-bit PCM via `commonFormat: .pcmFormatInt16` silently truncates the frame count (a 44100-frame file came back as 42969 frames — confirmed via both `AVAudioFile` and raw `ExtAudioFile`), and a WAV file `AVAudioFile` itself writes (with its automatic JUNK/FLLR alignment padding) comes back with `length == 0` when reopened. Full repro notes are in `WavCodec.swift`'s header comment. `WavCodec.swift`/`AiffCodec.swift` read and write raw PCM bytes directly instead — no framework in between, nothing to silently mis-round.

## Building

```sh
# Whole app (Core + Audio + App)
swift build
swift run          # launches the app
swift test          # WAV/AIFF round-trip tests

# C++ core only, no Swift toolchain needed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/Tests/CoreTests/AkaizerCoreTests
```

Set `AKAIZER_AUTOLOAD_PATH=/path/to/file.wav` before launching to skip the Open panel and load a file automatically on startup — added because `NSOpenPanel`'s accessibility tree proved unautomatable in a sandboxed terminal (clicks into its column browser and synthetic keystrokes into its fields never registered, even though every control in the app's own window did). Harmless for normal use; only fires when the env var is set.

## Distribution

```sh
Scripts/build_app_bundle.sh
open "dist/Akaizer S.app"
```

Builds the release binary and assembles a real `.app` bundle — `Contents/MacOS/AkaizerS`, `Contents/Info.plist`, `Contents/Resources/AppIcon.icns` — rather than the bare Mach-O executable `swift build` produces on its own. `Resources/Info.plist` and `Resources/AppIcon.icns` (generated once with a small Core Graphics/CoreImage script, not part of the repeatable build — the icon itself is a stretching waveform: tight on the left, wide on the right, in the same phosphor-green as `LCDReadoutView`) are checked into the repo; the script just copies them in alongside a fresh binary.

**Deliberately unsigned.** No Apple Developer ID certificate exists on this machine (`security find-identity -v -p codesigning` returns none) — and given that, unsigned (rather than an ad-hoc `codesign -s -`) was the explicit choice made for this project, distribution being a post-plan addition beyond the original 9 stages. The bundle runs cleanly on this Mac; copied to another one, Gatekeeper shows an "unidentified developer" prompt, overridden with right-click > Open. Real Developer ID signing + notarization (no prompt for anyone) needs an Apple Developer Program membership this project doesn't have.

A proper bundle is also what finally makes `NSApplication.ActivationPolicy` correct *without* the stage-4 `NSApplicationDelegate` workaround — LaunchServices sets `.regular` automatically from a real `Info.plist`. That workaround stays in `AkaizerSApp.swift` regardless: `swift run`/the raw binary (still the normal dev workflow) has no `Info.plist` and still needs it.

## Design notes worth knowing before touching StretchEngine.cpp / RealtimeStretchPlayer.cpp

- **Two engines, two threading models, on purpose.** `AkzStretchEngine`/`StretchProcessor` is synchronous and allocates on whatever thread calls `process()` — correct for offline rendering (Process/Save), unsafe on a CoreAudio render thread. `AkzRealtimePlayer`/`RealtimePlayer` wraps a background thread that does the same recompute and publishes via `std::atomic_load`/`store` on a `shared_ptr` so `pull()` never allocates or blocks — used only for live audition. Don't try to make one serve both jobs.
- **`pull()`'s shared_ptr publish is not strict wait-free lock-free audio programming** (libc++'s shared_ptr atomics use an internal spinlock) — a deliberate, documented tradeoff for a personal-use app. ThreadSanitizer-clean and stress-tested (`RealtimeStretchPlayerTests.cpp`), but don't build a shipped plugin's real-time path on this unmodified.
- **CLASSIC quantises output length to whole cycle-length blocks and does not correct the resulting timing error.** That imprecision is the documented, authentic behaviour ("perfect pitch, imprecise timing") — see `AkzEngine_Classic` in `AkaizerCore.h`. Don't "fix" it.
- **REVISED synthesises the same block-quantised audio, then linearly resamples to the exact requested length.** That resample is what introduces REVISED's documented slight pitch drift — it falls out of the implementation rather than being a bolted-on effect.
- **INTELLIGENT mode currently falls back to the CYCLIC synthesis path.** Splice-point analysis (autocorrelation/AMDF-driven boundaries, plus the `quality`/`width` parameters that only apply in this mode) is not yet implemented.
- **Every `AkzMachineProfile` field is cited** in `MachineProfile.cpp` as `[M]` (manual-confirmed), `[F]` (forum/secondary source), or `[I]` (this project's inference). Check the citation before changing a value.
