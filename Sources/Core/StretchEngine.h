// StretchEngine.h
//
// Internal C++ implementation behind the akz_stretch_engine_* C API. See
// AkaizerCore.h for the contract; see the project plan section 2 for the
// algorithm rationale.
//
// Current implementation status (build order stage 7): both CYCLIC and
// INTELLIGENT modes are implemented for both CLASSIC and REVISED engines.
// INTELLIGENT is a SOLA-style search: cross-correlation finds the best
// splice offset near each nominal read position instead of splicing at a
// fixed cycle length -- see _synthesizeIntelligent's comment and plan
// section 2.2.

#ifndef AKAIZER_STRETCH_ENGINE_H
#define AKAIZER_STRETCH_ENGINE_H

#include "include/AkaizerCore.h"
#include <vector>

namespace akz {

class StretchEngine {
public:
    explicit StretchEngine(double sampleRateHz);

    void setParams(const AkzStretchParams& params);
    void reset();
    void setSource(const float* sourceFrames, size_t frameCount);

    size_t process(float* outFrames, size_t maxOutFrames);
    size_t outputLength() const;

    // Cheap path for RealtimeStretchPlayer (see its worker loop): re-runs
    // ONLY the filter stage against the cached pre-filter buffer, instead
    // of re-running the whole stretch/resample/transpose pipeline. Valid
    // only when `params` differs from the params _recompute() last ran
    // with in no field other than filterCutoff01/filterResonance01 --
    // callers must check paramsDifferOnlyInFilter() themselves first, this
    // does not re-check it. Output length is unchanged by construction,
    // which is the whole point: it lets a live-audition read position stay
    // put across a filter-only change instead of restarting.
    void reapplyFilterOnly(const AkzStretchParams& params);

    // True when `a` and `b` differ in some field, but NOT in any field
    // other than filterCutoff01/filterResonance01. False if they're
    // identical (nothing to do) or if anything else differs (needs a full
    // _recompute()).
    static bool paramsDifferOnlyInFilter(const AkzStretchParams& a, const AkzStretchParams& b);

    // 2.1 feedback ("Still splitting channels and phasing in realtime
    // edit mode"): INTELLIGENT mode's SOLA splice search
    // (_synthesizeIntelligent) picks its offset by cross-correlating
    // THIS engine's own _quantizedSource -- run independently per
    // channel (offline: ProcessedRender.swift's per-channel loop;
    // realtime: one RealtimeStretchPlayer per channel), L and R
    // routinely choose different offsets at the same nominal position,
    // heard as stereo decorrelation/comb filtering. When a guide is set,
    // _synthesizeIntelligent uses offsets[iter] verbatim instead of
    // searching -- so every channel given the SAME guide (typically
    // derived from a mid/summed analysis pass over all channels) splices
    // at identical frame positions and the stereo image survives. Marks
    // the engine dirty (a guide change must force a re-render). A
    // mismatched length (fewer offsets than this render's own iteration
    // count) is not an error: iterations past the guide's end fall back
    // to their own independent search rather than reading out of bounds.
    void setSpliceGuide(const std::vector<long long>& offsets);
    void clearSpliceGuide();

    // The per-iteration splice offsets the last INTELLIGENT-mode
    // _recompute() actually used -- whether chosen by its own search
    // (no guide set) or supplied via setSpliceGuide(). Empty outside
    // INTELLIGENT mode, before any render, or after a source/params
    // change with inLen == 0. This is what a "guide pass" render (over a
    // mid/summed signal, no guide of its own) hands to setSpliceGuide()
    // on the real per-channel engines -- see ProcessedRender.swift and
    // LiveAuditionController.swift for the two coordinators.
    const std::vector<long long>& lastSpliceOffsets() const { return _lastSpliceOffsets; }

private:
    // Recomputes _output from _source and _params. Allocates -- this is
    // NOT real-time safe, and is only ever called from setSource() /
    // setParams(), never from process(). See "Real-time constraint" in
    // AkaizerCore.h: process() only ever reads from the already-computed
    // _output buffer.
    //
    // TODO(stage 4): for very long sources this precompute-everything
    // approach is wasteful of memory and adds latency before the first
    // process() call. Revisit as a true incremental/streaming synthesis
    // once real-time audition is wired up, if it proves necessary in
    // practice -- see plan "2.5 Streaming".
    void _recompute();

    // Reads _quantizedSource[index], returning 0.0f for any out-of-range
    // index. Centralises the boundary clamping the block-splice
    // algorithm needs at the start and end of the source.
    float _sourceAt(long long index) const;

    // The block-insert/delete-with-crossfade synthesis described in the
    // S3200XL manual (plan "2.1"): walks `numOutBlocks` output blocks of
    // `cycleLength` frames each, mapping output block k to input block
    // floor(k * numInBlocks / numOutBlocks) -- which duplicates input
    // blocks when lengthening and drops them when shortening -- and
    // crossfades the tail of each block into the head of the next so the
    // splice is not a hard edit. Appends exactly numOutBlocks * cycleLength
    // frames to `out`.
    void _synthesizeCyclicBlocks(std::vector<float>& out, size_t numOutBlocks, int cycleLength) const;

    // SOLA-style splice-point search (plan "2.2", INTELLIGENT mode): see
    // the .cpp for the full derivation. Appends the synthesised result to
    // `out` (its own frame plus per-iteration overlap-added segments, not
    // a fixed frame count like the CYCLIC path above). Not const: records
    // every chosen offset into _lastSpliceOffsets, and consults
    // _spliceGuide/_hasSpliceGuide when set (see setSpliceGuide above).
    void _synthesizeIntelligent(std::vector<float>& out, double ratio, int quality, int width);

    double _sampleRateHz;
    AkzStretchParams _params;
    std::vector<float> _source;          // exactly what setSource() was given, never mutated
    std::vector<float> _quantizedSource; // _source re-quantised to the current machine's bit depth (build order stage 6) -- recomputed fresh from _source every _recompute(), so switching machines never compounds quantisation from a previous one
    std::vector<float> _preFilter; // _output as it stood right before applyFilter() in the last _recompute() -- lets reapplyFilterOnly() redo just the last stage
    std::vector<float> _output;
    size_t _readPos = 0;   // how many output frames already handed to the caller via process()
    bool _dirty = true;    // true when _output needs recomputing before the next process() call

    // 2.1 stereo splice linkage -- see setSpliceGuide/lastSpliceOffsets above.
    std::vector<long long> _spliceGuide;
    bool _hasSpliceGuide = false;
    std::vector<long long> _lastSpliceOffsets;
};

} // namespace akz

#endif // AKAIZER_STRETCH_ENGINE_H
