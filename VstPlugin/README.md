# Patina FX

A VST3 insert effect that runs Patina's heritage-sampler signal path —
rate/bandwidth, bit depth, and filter — live on a DAW channel. Built on
the same C++17 DSP core (`Sources/Core`) the macOS app uses, through a
new block-streaming entry point (`RealtimeChannel` /
`akz_realtime_channel_*`) added specifically for this: the app's own
offline render and live-audition player both need a whole sample known
in advance, which a live insert effect never has.

**Time-stretch and transpose/varispeed are not present in this plugin.**
Both need the whole audio signal known ahead of time (stretch: a global
block-splice search or mapping over the full buffer; transpose: it
changes the audio's duration, which an insert effect can't do without a
growing/draining buffer) — see `Sources/Core/RealtimeChannel.h`'s header
comment.

## What it does

One machine (from the same ten-machine heritage roster as the main app)
colors the channel's audio through:

1. **Rate/bandwidth** — the machine's anti-alias + decimate/hold front
   end (fixed per machine, or a live "Bandwidth" knob on S900/S950).
2. **Bit depth** — the machine's native converter, with a live override
   (1–24 bits) on top; companding (Emulator II's mu-law) is always the
   machine's own.
3. **Filter** — the machine's own topology (one-pole cascade, Chamberlin/
   TPT state-variable, or SSM-style ladder), with live Cutoff and (where
   the machine has one) Resonance.

Switching machines crossfades over ~10ms so a mid-performance swap
doesn't click. Every other control (bandwidth, bit depth, cutoff,
resonance) retunes in place with no re-render and no gap.

## Building

Requires CMake ≥ 3.22 and a C++17 compiler. JUCE 8.0.15 is fetched
automatically via `FetchContent` — the first configure needs network
access and takes a while (JUCE is a large repository even shallow-cloned).

```sh
cmake -S VstPlugin -B VstPlugin/build -DCMAKE_BUILD_TYPE=Release
cmake --build VstPlugin/build --config Release
```

Outputs (under `VstPlugin/build/PatinaFX_artefacts/Release/`):

- `VST3/Patina FX.vst3` — copy to `%COMMONPROGRAMFILES%\VST3\` on
  Windows (`/Library/Audio/Plug-Ins/VST3/` on macOS) to make it visible
  to hosts.
- `Standalone/Patina FX.exe` (or `.app` on macOS) — runs without a host,
  useful for quick manual testing.

### Windows 11 specifics

Build with Visual Studio 2022 (the Ninja or Visual Studio CMake
generator both work). The plugin links the MSVC static runtime
(`/MT` / `MultiThreaded`, set in `CMakeLists.txt`), so the built VST3
has no VC++ redistributable dependency. `_USE_MATH_DEFINES` is defined
for the `AkaizerCore` target only when building with MSVC — the shared
core itself is untouched; every other consumer's compiler (Clang on
macOS, the Swift toolchain) already defines `M_PI` unconditionally.

```powershell
cmake -S VstPlugin -B VstPlugin\build -G "Visual Studio 17 2022" -A x64
cmake --build VstPlugin\build --config Release
```

#### Offline build (no network on the build machine)

The normal flow above `FetchContent`-clones JUCE from GitHub on first
configure, which needs network access. To build on a machine with none:

1. On a machine with network access, clone JUCE at the pinned tag and
   copy the checkout to the offline machine (any path, e.g.
   `C:\deps\JUCE`):
   ```sh
   git clone --branch 8.0.15 --depth 1 https://github.com/juce-framework/JUCE.git
   ```
2. Copy this repo's `Sources/Core/` and `VstPlugin/` directories to the
   offline machine, same relative layout to each other.
3. On the offline machine, install Visual Studio 2022 (Desktop
   development with C++ workload) and CMake ≥ 3.22 — both installers,
   no network needed after download.
4. Configure with `JUCE_SOURCE_DIR` pointing at the copied JUCE
   checkout, which skips the git clone entirely:
   ```powershell
   cmake -S VstPlugin -B VstPlugin\build -G "Visual Studio 17 2022" -A x64 -DJUCE_SOURCE_DIR="C:/deps/JUCE"
   cmake --build VstPlugin\build --config Release
   ```

This plugin has **not** been built or run on actual Windows 11 — only
on macOS, where the same CMake project also produces VST3/AU/Standalone
builds for local development. Before shipping:

- Build on Windows 11 with VS 2022.
- Run [pluginval](https://github.com/Tracktion/pluginval) at strictness
  level 8+ against the built VST3 (covers threading, automation, state
  round-trip, and odd buffer sizes).
- Load it in a real host (e.g. REAPER), insert it on an audio track and
  an instrument track, automate every parameter, and save/reload the
  project.

## Parameters

| Parameter  | Range           | Notes                                             |
|------------|-----------------|----------------------------------------------------|
| Machine    | 10 choices      | Saved by stable id, not index — safe across reorder |
| Bit Depth  | 0 (native)–24   | 0 = machine's own native depth                     |
| Bandwidth  | 1000–48000 Hz   | Only audible on S900/S950; harmless (clamped) elsewhere |
| Cutoff     | 0–1             | Logarithmic 20 Hz–Nyquist, matches the app's own knob |
| Resonance  | 0–1             | Only audible on machines with a resonant filter    |
| Mix        | 0–1             | Dry/wet                                            |
| Output     | -24–+24 dB      | Post-mix trim                                      |

## Source layout

```
VstPlugin/
  CMakeLists.txt          # own top-level CMake project; does not touch the repo root's
  Source/
    PluginProcessor.h/.cpp  # AudioProcessor: owns one AkzRealtimeChannel per audio channel
    PluginEditor.h/.cpp     # machine combo + knobs; visibility ported from MachineControls.swift
    MachineControls.h/.cpp  # which knobs a machine shows, from AkzMachineProfile capability flags
```
