# Patina

A native Apple Silicon app that recreates the time-stretch, converter, and filter character of classic hardware samplers — Akai (S900/S950/S1000/S2000/S3000/S3200), E-mu (SP-1200, Emulator II), Fairlight (CMI IIx), and Ensoniq (Mirage) — with real-time audition and per-machine modelling.

This is a clean-room implementation: no original manufacturer code, no ROM disassembly. The DSP is built from published service/owner's manuals and datasheets, validated by ear against real hardware behaviour. Full development history, per-stage rationale, and citation notes live in [`HISTORY.md`](HISTORY.md).

## Features

- **10-machine roster**, grouped by manufacturer, each with cited (or explicitly flagged inferred) converter/filter/rate behaviour
- **Time-stretch** (Akai-capable machines only) — CLASSIC, REVISED, and INTELLIGENT (SOLA) modes, with per-machine cycle/quality/width controls
- **Transpose/varispeed** — zero-order-hold or linear interpolation, matched per machine
- **Converter + filter modelling** — native bit-depth quantisation, machine-appropriate VCF (resonant SVF or cascaded lowpass, plus a ladder filter for the Emulator II)
- **Sample-rate/bandwidth modelling** — real anti-alias + decimation + DAC reconstruction stages
- **Real-time audition** — live preview while dragging any knob, glitch-free across channels and parameter changes
- **Non-destructive workflow** — Undo/redo, Revert, Defaults, A/B against the original, drag-and-drop load/export
- **Presets** — named parameter sets, persisted per machine
- **Waveform display** — original/processed overlay, scrubbable playhead, draggable start point
- **Settings carry across machine switches** — clamped if the target machine can show them, parked untouched if not

## Requirements

- macOS (Apple Silicon), Xcode 26+
- Swift 5.9+ / Swift Package Manager
- No third-party dependencies

## Building

```sh
swift build
swift run           # launches the app
swift test          # WAV/AIFF round-trip + unit tests

# C++ core only, no Swift toolchain needed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/Tests/CoreTests/AkaizerCoreTests
```

## Distribution

```sh
Scripts/build_app_bundle.sh
open "dist/Patina.app"
```

Assembles a real, iconed `.app` bundle. Deliberately unsigned — no Apple Developer ID on this machine; Gatekeeper shows an "unidentified developer" prompt on another Mac, overridden with right-click > Open.

## Project Structure

```
patina/
├── CMakeLists.txt          Standalone build for the C++ core + tests
├── Package.swift           Builds the whole app (Core + Audio + App)
├── Sources/
│   ├── Core/                C++ DSP engine — no Apple frameworks, portable
│   ├── App/                 SwiftUI views, app lifecycle
│   └── Audio/                WAV/AIFF codecs, Swift/C++ bridging
├── Resources/               Checked-in app bundle assets (Info.plist, icon)
├── Scripts/                 build_app_bundle.sh
└── Tests/
    ├── CoreTests/            C++ unit tests
    ├── AudioTests/           XCTest — codec round-trip, stretch chain
    └── Fixtures/             Reference audio
```

## Tech Stack

- **C++17** — DSP core, portable, no Apple framework dependencies
- **Swift + SwiftUI** — app shell
- **Hand-rolled WAV/AIFF codecs** — `AVAudioFile` has reproducible bit-exactness bugs on this SDK; details in `HISTORY.md`
- **CMake 3.14+** — builds/tests the C++ core standalone
- **Swift Package Manager** — builds the whole app; no `.xcodeproj` maintained
- Zero third-party dependencies, by design

## Full History

See [`HISTORY.md`](HISTORY.md) for the complete build-stage log, every bug found and fixed, known limitations, and design notes for anyone touching `StretchEngine.cpp` / `RealtimeStretchPlayer.cpp`.
