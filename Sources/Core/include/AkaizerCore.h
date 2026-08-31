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

// The heritage samplers this app models. v1 was Akai-only (S900 has no
// time-stretch capability, added in the S950, but is included for
// converter/filter character); v2's heritage-roster plan adds four
// non-Akai machines, none of which have time-stretch at all -- they
// earn their place through converter, filter, varispeed and sample-
// rate/bandwidth character instead. Append-only, NEVER renumbered --
// PresetStore.swift's v1 migration table maps old raw values to stable
// string ids that depend on this ordering never changing retroactively;
// a genuinely new machine always goes immediately before AkzMachine_Count.
typedef enum AkzMachine {
    AkzMachine_S900  = 0,
    AkzMachine_S950  = 1,
    AkzMachine_S1000 = 2,
    AkzMachine_S2000 = 3,
    AkzMachine_S3000 = 4,
    AkzMachine_S3200 = 5,
    AkzMachine_SP1200      = 6, // E-mu SP-1200 (1987) -- fixed 26.04kHz, 12-bit linear, drop-sample (no interpolation)
    AkzMachine_FairlightCmi2x = 7, // Fairlight CMI IIx (~1983) -- 8-bit linear, rate = 128 x pitch, pitch-tracking VCF
    AkzMachine_Mirage      = 8, // Ensoniq Mirage (1984) -- 8-bit unsigned, phase-accumulator drop-sample, CEM3328 per-voice filter
    AkzMachine_EmulatorII  = 9, // E-mu Emulator II (1984) -- 27.7kHz, AM6072 mu-law companding DAC, SSM2045 per-channel ladder filter
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

// Filter topology. Replaces v1's overloaded pair (filterHasResonance
// picking a class, filterSlopeDbPerOctave >= 24.0 picking a stage count)
// with an explicit selector, so FilterModel.cpp's factory dispatches on
// a capability field rather than growing a third if-branch per new
// machine family -- see FilterModel.h for what each topology models and
// which real chips it stands in for.
typedef enum AkzFilterTopology {
    AkzFilterTopology_OnePoleCascade    = 0, // cascaded 1-pole lowpass stages, no resonance -- S900/S950 (analog SC Butterworth stand-in), S1000 (digital moving lowpass)
    AkzFilterTopology_ChamberlinSvf     = 1, // naive Chamberlin SVF, k<=1.1 stability clamp -- v1/v2-stage2 S2000/S3000/S3200; superseded by TptSvf for these three from stage 6 on
    AkzFilterTopology_TptSvf            = 2, // zero-delay-feedback SVF, unconditionally stable to Nyquist, unity passband gain by construction -- what S2000/S3000/S3200 migrate to
    AkzFilterTopology_SsmLadder         = 3, // SSM2044/SSM2045-class 4-pole transistor ladder
    AkzFilterTopology_CemStateVariable  = 4, // CEM3320/3328-class per-voice resonant state-variable filter
    AkzFilterTopology_SwitchedCapacitor = 5  // switched-capacitor LPF/HPF pair (e.g. Fairlight's input anti-alias stage)
} AkzFilterTopology;

// ---------------------------------------------------------------------------
// Machine profile — read-only per-machine constants
// ---------------------------------------------------------------------------

typedef struct AkzMachineProfile {
    const char* name;                 // e.g. "S950"

    // Stable identifier, e.g. "akai.s950" -- what presets store on disk
    // (PresetStore.swift's AkaizerPreset.machineId), so that renumbering
    // or reordering AkzMachine never remaps a saved preset to a
    // different machine. Never renamed once shipped, same discipline as
    // AkzMachine itself never being renumbered.
    const char* stableId;

    // Roster metadata (v2 heritage-roster plan, stage 9) -- grouping/
    // sorting for the sidebar's machine browser, once the roster grows
    // past a flat picker. Display-only; no DSP reads these.
    const char* manufacturer;  // e.g. "Akai", "E-mu", "Roland", "Fairlight", "Ensoniq"
    int         yearIntroduced;

    // Sample rate. S900/S950 are continuously variable via an audio
    // bandwidth control (fs = bandwidth * 2.5); S1000 and later select
    // between exactly two fixed rates (22050/44100 Hz), captured here as
    // the same [min, max] pair even though the real hardware has nothing
    // playable in between -- RateModel.cpp's resolveSampleRateHz() clamps
    // a requested rate into this range without knowing (or enforcing)
    // that distinction, a known simplification, not a citation that
    // continuous values between them are real. minSampleRateHz ==
    // maxSampleRateHz only for a genuinely single-fixed-rate machine
    // (none of the current six -- watch for this when adding one that IS).
    double minSampleRateHz;
    double maxSampleRateHz;
    int    hasVariableSampleRate;     // 1 for S900/S950 (continuous), 0 for a discrete choice (S1000 and later) -- a UI hint (knob vs picker), not read by RateModel itself

    // Anti-alias filter, the ADC-side stage RateModel.cpp's
    // applyRecordPath runs before decimating to a rate below hostRateHz.
    // Modelled as a tracking filter (cutoff = target rate *
    // aaFilterCutoffRatio) rather than a fixed Hz value, matching how
    // the real S900/S950 bandwidth control is documented to move the
    // input filter and the sample clock together -- see MachineProfile.cpp
    // for what's cited vs inferred per machine. 0.5 = cutoff sits exactly
    // at the new Nyquist (little foldover); higher values leave the
    // cutoff above Nyquist, letting content above it fold back down --
    // that "deficiency" is real character on a cheap ADC, not a bug.
    double aaFilterCutoffRatio;
    int    aaFilterPoles;             // one-pole-cascade pole count approximating the real filter's slope, same non-precision-Butterworth caveat as FilterModel.h

    // Converter
    int    bitDepth;                  // 12 for S900/S950, 16 for S1000/S2000/S3000, 16 or 18 for S3200
    int    companded;                 // always 0 — none of these machines compand

    // Filter. filterHasResonance is a UI/capability flag ("does this
    // machine expose a resonance knob") -- FilterModel.cpp's dispatch
    // itself switches on filterTopology, not this flag, since a third
    // topology could have resonance without being a ChamberlinSvf/
    // TptSvf. filterSlopeDbPerOctave keeps its literal meaning (used
    // directly as pole count for OnePoleCascade; documentation-only for
    // the other topologies, whose pole count is a property of the real
    // chip, not a tunable). filterStageCount replaces the old
    // ">= 24.0 dB/oct" heuristic for S3200's second series SVF stage.
    int    filterHasResonance;        // 0 for S900/S950/S1000, 1 for S2000/S3000/S3200
    double filterSlopeDbPerOctave;    // 36 analog (S900/S950), 18 digital (S1000), 12 digital SVF (S2000/S3000), 24 (S3200 w/ 2nd filter)
    int    filterTracksPitch;         // 1 only for S900/S950 (per-voice analog filter clocked with the voice)
    AkzFilterTopology filterTopology; // which class FilterModel.cpp's factory instantiates
    int    filterStageCount;          // stages of filterTopology run in series (2 for S3200's "2nd DIGITAL FILTER" -> 24dB/oct, 1 otherwise)

    // Resonant-peak compensation for AkzFilterTopology_TptSvf (v2
    // heritage-roster plan, "SVF fix" stage). [I] -- no manual specifies
    // this; it exists to fix the passband-gain clipping that
    // ChamberlinSvf's k <= 1.1 stability clamp left behind. 0 = no
    // compensation (the resonant peak, a real and kept characteristic,
    // passes through at full height, closer to hardware that itself
    // gets louder at resonance); 1 = full compensation (output peak
    // held at unity even at maximum resonance, closer to hardware with
    // its own internal limiting). Ignored for every other topology.
    double filterResonanceCompensation01;

    // DAC back end (v2 heritage-roster plan, stage 5 -- RateModel.cpp's
    // applyDacPath). 1 when the machine's D/A conversion clock is the
    // SAME clock that sets pitch (S900/S950 -- "per-voice DAC clock is
    // varied directly," the same physical fact filterTracksPitch already
    // captures for these two); 0 when conversion runs at a fixed
    // native-rate clock regardless of transpose (S1000's "fixed passive
    // LC reconstruction... not by pitch," S2000/S3000/S3200's "runs at
    // fixed 44.1kHz after pitch interpolation on the real chip" -- see
    // FilterModel.h). A separate field from filterTracksPitch on
    // purpose, not just a rename of it: the two happen to agree for
    // every machine here (one physical clock, two consequences), but a
    // future machine could plausibly have its VCF and its DAC clock
    // behave differently.
    int    dacClockTracksPitch;

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

// Number of machines in the roster -- AkzMachine_Count, exposed across
// the C boundary so Swift's StretchProcessor.allMachines can be
// (0..<akz_machine_count()) rather than a hand-maintained literal array
// that has to be kept in sync with the enum by hand.
size_t akz_machine_count(void);

// ---------------------------------------------------------------------------
// Provenance — is a modelled stage cited, or this project's inference?
// ---------------------------------------------------------------------------
//
// v1 tracked this in source comments only (`[M]`/`[I]`/`[M/I]` tags in
// MachineProfile.cpp). v2's fidelity bar is citation-first but allows
// inference IF IT IS VISIBLE TO THE USER, not just to a reader of the
// source -- this table is what the UI reads to show that (an inference
// badge on a knob, a "modelled from..." panel). Every AkzMachine x
// AkzStage pair has an entry, including AkzProvenanceLevel_Unmodelled
// for a stage this build's DSP doesn't implement yet (e.g. Rate/Dac
// before the heritage-roster plan's stage 4/5 land) -- enforced by a
// completeness test in MachineProfileTests.cpp, not left to be missing.

typedef enum AkzStage {
    AkzStage_Rate         = 0, // sample-rate/bandwidth front end (decimate + reconstruct)
    AkzStage_Converter    = 1, // bit-depth quantisation, companding, transfer curve
    AkzStage_Filter       = 2, // the machine-appropriate VCF
    AkzStage_Interpolator = 3, // transpose/varispeed interpolation kind
    AkzStage_Stretch      = 4, // time-stretch algorithm (n/a on machines with none -- Unmodelled, correctly)
    AkzStage_Dac          = 5, // DAC back end (zero-order hold + reconstruction filter)
    AkzStage_Count
} AkzStage;

typedef enum AkzProvenanceLevel {
    AkzProvenanceLevel_Measured    = 0, // this project measured the real hardware directly
    AkzProvenanceLevel_Manual      = 1, // a service/owner's manual, datasheet, or MAME-confirmed behavioural reference states this directly
    AkzProvenanceLevel_Inferred    = 2, // this project's own inference or approximation, flagged as such
    AkzProvenanceLevel_Unmodelled  = 3  // not implemented by this build's DSP yet, regardless of citation quality
} AkzProvenanceLevel;

typedef struct AkzStageProvenance {
    AkzProvenanceLevel level;
    const char* note; // short human-readable citation or inference note, e.g. "S950 manual: fs = bandwidth * 2.5"
} AkzStageProvenance;

// Returns provenance for one machine/stage pair. Never NULL -- every
// combination has an entry (see completeness test above). The returned
// pointer is to static storage, like akz_machine_profile.
const AkzStageProvenance* akz_machine_stage_provenance(AkzMachine machine, AkzStage stage);

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

    // Sample rate / bandwidth front end (heritage-roster plan v2, stage
    // 3 schema -- the RateModel DSP itself lands in stage 4). 0.0 means
    // "use the machine's own default" (profile.maxSampleRateHz for a
    // fixed-rate machine; the manual-documented default bandwidth for a
    // variable-rate one, e.g. the S950's own top bandwidth), resolved by
    // whatever consumes this field rather than requiring every caller to
    // already know the machine's range. A non-zero value is silently
    // clamped into [minSampleRateHz, maxSampleRateHz] by that same
    // resolution step. Appended (not inserted) so every existing
    // AkzStretchParams literal in Swift keeps compiling positionally --
    // see StretchBridge.swift.
    float sampleRateHz;
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

// Render-thread safe, non-blocking. Non-zero WHILE the background worker
// is actively re-rendering (from the moment a change is confirmed dirty
// until its result publishes), zero at rest. Unlike
// akz_realtime_player_is_ready above -- which latches non-zero forever
// after the first publish and so can never report a later re-render --
// this one genuinely toggles, for a "recomputing" UI indicator on a slow
// re-render (a known gap flagged in the project README before this was
// added: no visual cue during live audition's background recompute).
int akz_realtime_player_is_recomputing(const AkzRealtimePlayer* player);

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
