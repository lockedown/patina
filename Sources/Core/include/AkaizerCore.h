// AkaizerCore.h
//
// C-compatible entry point for the Akaizer S DSP core. This is the only
// header the Swift layer (or a unit test) is allowed to include — everything
// else under Sources/Core is a C++ implementation detail. Keeping the
// boundary C-shaped means the core has no dependency on Objective-C++,
// AVFoundation, or any Apple framework, so it builds and tests standalone.
//
// The core is a *streaming, pull-based* processor: the caller asks for N
// output frames at a time via akz_stretch_process(), and the same call is
// used whether driving an AVAudioSourceNode in real time or rendering a
// file offline as fast as possible. That symmetry is deliberate — see the
// project plan, "2.5 Streaming" — offline and real-time renders must be
// bit-identical.

#ifndef AKAIZER_CORE_H
#define AKAIZER_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Machines
// ---------------------------------------------------------------------------

// The Akai samplers this app models. S900 has no time-stretch capability
// (added in the S950) but is included for converter/filter character.
typedef enum AkzMachine {
    AkzMachine_S900  = 0,
    AkzMachine_S950  = 1,
    AkzMachine_S1000 = 2,
    AkzMachine_S2000 = 3,
    AkzMachine_S3000 = 4,
    AkzMachine_S3200 = 5,
    AkzMachine_Count
} AkzMachine;

// Time-stretch analysis mode. Only S950 lacks a mode switch (it always
// behaves like Cyclic, gated by Mon1/Pol2 instead) — see MachineProfile.
typedef enum AkzStretchMode {
    AkzStretchMode_Cyclic       = 0, // fixed block size, constant repeat rate
    AkzStretchMode_Intelligent  = 1  // splice points chosen by signal analysis
} AkzStretchMode;

// CLASSIC reproduces the original's length-quantisation error (perfect
// pitch, imprecise timing). REVISED hits the exact requested length by
// nudging block boundaries, which is what drifts the pitch slightly.
// See project plan, "2.3 Reproduce the timing error — do not fix it."
typedef enum AkzEngine {
    AkzEngine_Classic = 0,
    AkzEngine_Revised = 1
} AkzEngine;

// ---------------------------------------------------------------------------
// Machine profile — read-only per-machine constants
// ---------------------------------------------------------------------------

typedef struct AkzMachineProfile {
    const char* name;                 // e.g. "S950"

    // Sample rate. S900/S950 are continuously variable via an audio
    // bandwidth control (fs = bandwidth * 2.5); S1000 and later are fixed
    // to one of a small set of rates. minSampleRateHz == maxSampleRateHz
    // for the fixed-rate machines.
    double minSampleRateHz;
    double maxSampleRateHz;
    int    hasVariableSampleRate;     // 1 for S900/S950, 0 otherwise

    // Converter
    int    bitDepth;                  // 12 for S900/S950, 16 for S1000/S2000/S3000, 16 or 18 for S3200
    int    companded;                 // always 0 — none of these machines compand

    // Filter
    int    filterHasResonance;        // 0 for S900/S950/S1000, 1 for S2000/S3000/S3200
    double filterSlopeDbPerOctave;    // 36 analog (S900/S950), 18 digital (S1000), 12 digital SVF (S2000/S3000), 24 (S3200 w/ 2nd filter)
    int    filterTracksPitch;         // 1 only for S900/S950 (per-voice analog filter clocked with the voice)

    // Transposition / interpolation. See MachineProfile.cpp for citations.
    int    interpolatorOrder;         // 0 = none (S900/S950 vary the DAC clock directly), 1 = zero-order hold, 2 = linear

    // Time stretch
    int    supportsTimeStretch;       // 0 only for S900
    double maxStretchPercent;         // 999 for S950, 2000 for S1000/S2000/S3000/S3200
    int    hasModeSwitch;             // 0 for S950 (no Cyclic/Intelligent switch), 1 otherwise
    int    hasZoneSelect;             // 1 for S1000/S3000/S3200, 0 for S950/S2000
    int    defaultCycleLength;        // in samples: 1000 (S950 D-time), 1000 (S1000), 1340 (S2000), 1000 (S3000/S3200)

    // Memory model (advisory only — see plan "3.5 Memory model")
    int64_t memoryBudgetSamplePoints; // fixed sample-point budget, independent of rate. 0 = not modelled (S2000+)
} AkzMachineProfile;

// Returns the profile for a machine. The returned pointer is to static
// storage and never needs to be freed.
const AkzMachineProfile* akz_machine_profile(AkzMachine machine);

// ---------------------------------------------------------------------------
// Stretch parameters
// ---------------------------------------------------------------------------

typedef struct AkzStretchParams {
    AkzMachine     machine;
    AkzEngine      engine;
    AkzStretchMode mode;             // ignored (treated as Cyclic) when the machine has no mode switch

    float timeFactorPercent;         // 25..2000 (25..999 for S950), 100 = no change
    int   cycleLengthSamples;        // D-time / Cycle length. Only meaningful in Cyclic mode.
    int   quality;                   // 0..99. Only meaningful in Intelligent mode.
    int   width;                     // 0..99, crossfade width. Only meaningful in Intelligent mode.

    // Transpose (build order stage 5). Applied AFTER the time-stretch
    // step, matching the signal chain in the project plan ("4. Signal
    // chain"). This is varispeed, not pitch-shift-without-duration-
    // change: pitch and duration move together, same as the real
    // hardware. -36..36, 0 = no change. ±36 matches Akaizer v2.5's
    // range; no machine-specific range is modelled yet -- see
    // Interpolator.h for the per-machine interpolation quality
    // (zero-order hold vs linear) this actually applies.
    float transposeSemitones;

    // Filter (build order stage 6). Applied AFTER transpose, matching
    // the signal chain. 0..1, logarithmically mapped to 20 Hz..Nyquist
    // by FilterModel -- 1.0 = fully open (the hardware's own "0xffff =
    // Nyquist" convention). filterResonance01 (0..1, mapped to the SVF's
    // 4-bit resonance code) is ignored on machines without resonance
    // (AkzMachineProfile.filterHasResonance == 0) -- see FilterModel.h.
    float filterCutoff01;
    float filterResonance01;
} AkzStretchParams;

// Fills params with the machine's documented defaults.
void akz_stretch_params_default(AkzMachine machine, AkzStretchParams* out_params);

// ---------------------------------------------------------------------------
// Stretch engine — opaque handle
// ---------------------------------------------------------------------------

typedef struct AkzStretchEngine AkzStretchEngine;

// Creates a stretch engine for mono float32 audio at the given machine's
// internal sample rate. Ownership passes to the caller; free with
// akz_stretch_engine_destroy().
AkzStretchEngine* akz_stretch_engine_create(double sampleRateHz);
void              akz_stretch_engine_destroy(AkzStretchEngine* engine);

// Applies new parameters. Safe to call between process() calls; NOT
// real-time safe to call concurrently with process() from another thread —
// the caller is responsible for the lock-free handoff described in the
// project plan ("Real-time constraint").
void akz_stretch_engine_set_params(AkzStretchEngine* engine, const AkzStretchParams* params);

// Resets internal state (as if a new sample had been loaded).
void akz_stretch_engine_reset(AkzStretchEngine* engine);

// Feeds the entire source buffer to the engine ahead of pull-based
// rendering. The engine copies what it needs; the caller retains ownership
// of source_frames. Must be called after reset() and before the first
// process() call for a given sample.
void akz_stretch_engine_set_source(AkzStretchEngine* engine, const float* source_frames, size_t frame_count);

// Renders up to max_out_frames of processed output into out_frames.
// Returns the number of frames actually written; a return value less than
// max_out_frames means the source is exhausted. This is the ONLY entry
// point used for both real-time audition and offline rendering — see the
// header comment above.
size_t akz_stretch_engine_process(AkzStretchEngine* engine, float* out_frames, size_t max_out_frames);

// The exact total output length the engine will produce for the current
// source and params, computed up front. For AkzEngine_Classic this is
// round(n_in * factor / cycle) * cycle — the length-quantisation error is
// deliberate, see plan "2.3". For AkzEngine_Revised this equals
// round(n_in * factor) exactly.
size_t akz_stretch_engine_output_length(const AkzStretchEngine* engine);

// ---------------------------------------------------------------------------
// Real-time audition player — build order stage 4
// ---------------------------------------------------------------------------
//
// AkzStretchEngine above is fully synchronous: process() recomputes on
// whatever thread calls it if params/source changed since the last call.
// That is exactly right for offline rendering (the app's Process/Save
// buttons) and is why the streaming/offline bit-identity test in
// Tests/CoreTests/StretchEngineTests.cpp works — but it is NOT safe to
// drive from a CoreAudio render thread: a slider drag could land the
// (allocating) recompute directly on the audio thread.
//
// AkzRealtimePlayer is the render-thread-safe counterpart, used only for
// live audition. It owns a background thread that does the same
// recompute work AkzStretchEngine does, and publishes the result via a
// shared_ptr swap (std::atomic_load/store on shared_ptr, not
// atomic<shared_ptr<T>> — this project targets C++17) so pull() never
// allocates and never blocks: it takes a strong reference to whatever is
// currently published, reads from it, and lets the reference drop. This
// is NOT strict wait-free lock-free audio programming (libc++'s
// shared_ptr atomic ops use an internal spinlock) — a deliberate,
// documented tradeoff for a personal-use app, not something to build a
// shipped plugin's real-time path on unmodified. See RealtimeStretchPlayer.h.
//
// pull() loops the most recently published render continuously, which is
// the right behaviour for "drag a slider and hear it" auditioning, not
// one-shot playback. Every setParams()/setSource() call restarts the loop
// from the top of the newly published buffer once it's ready -- audition
// is not expected to be phase-continuous across a parameter change.

typedef struct AkzRealtimePlayer AkzRealtimePlayer;

AkzRealtimePlayer* akz_realtime_player_create(double sampleRateHz);
void                akz_realtime_player_destroy(AkzRealtimePlayer* player);

// Main/UI-thread only (copies source_frames, may allocate). Requests a
// background recompute; does not block waiting for it to finish.
void akz_realtime_player_set_source(AkzRealtimePlayer* player, const float* source_frames, size_t frame_count);

// Main/UI-thread only (may allocate/lock). Requests a background
// recompute with new params; does not block waiting for it to finish.
void akz_realtime_player_set_params(AkzRealtimePlayer* player, const AkzStretchParams* params);

// Render-thread safe: never allocates, never blocks on the background
// worker. Always fills exactly max_out_frames -- with looped audio once
// a render has published, with silence before the first one has.
size_t akz_realtime_player_pull(AkzRealtimePlayer* player, float* out_frames, size_t max_out_frames);

// Render-thread safe, non-blocking. Non-zero once at least one render has
// published (pull() will produce real audio rather than silence).
int akz_realtime_player_is_ready(const AkzRealtimePlayer* player);

// Render-thread safe, non-blocking. Non-zero once a stretch-affecting
// re-render (Transpose/Stretch/Cycle/Quality/Width/Mode -- anything that
// can change buffer length) has finished on the worker thread and is
// waiting to be swapped in via akz_realtime_player_commit_pending().
// Filter/resonance-only changes never appear here -- they publish
// immediately, with no commit step, since they can't change length or
// need cross-channel coordination. See LiveAuditionController.swift's
// render callback for why this two-step publish exists: with N
// independent players (one per channel, each with its own worker
// thread), publishing a length-changing re-render the instant ITS OWN
// worker finishes lets one channel start playing new audio while a
// sibling channel is still finishing the SAME change on the OLD audio --
// heard as artificial stereo width. The caller must check this on every
// channel and only commit once ALL of them are ready, in the same
// render-callback invocation, so every channel's read position resets
// to 0 on the exact same audio frame.
int akz_realtime_player_has_pending_commit(const AkzRealtimePlayer* player);

// Render-thread safe. Swaps the pending re-render (if any) into the
// published buffer and resets the read position to 0; a no-op if
// akz_realtime_player_has_pending_commit() was false. See that
// function's comment -- callers must gate this on every sibling
// channel's player also being ready, and commit all of them together.
void akz_realtime_player_commit_pending(AkzRealtimePlayer* player);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AKAIZER_CORE_H
