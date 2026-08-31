// StretchEngine.cpp
//
// See StretchEngine.h for the algorithm rationale and implementation
// status. Two things worth restating while reading this file:
//
//   1. CLASSIC quantises output length to a whole number of cycle-length
//      blocks and does NOT correct for the resulting error -- that error
//      is the documented behaviour ("perfect pitch, imprecise timing"),
//      not a bug. See plan "2.3 Reproduce the timing error -- do not fix
//      it."
//   2. REVISED synthesises the same block-quantised audio internally, then
//      linearly resamples it to the exact requested length. That resample
//      is what introduces REVISED's documented slight pitch drift -- it
//      is a direct, faithful consequence of "hitting exact timing" rather
//      than a separate effect bolted on.

#include "StretchEngine.h"
#include "FilterModel.h"
#include "Interpolator.h"
#include "MachineProfile.h"
#include "RateModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace akz {

namespace {

// Below this, block-based splicing has nothing meaningful to crossfade
// and "cycle length" stops being a meaningful concept.
constexpr int kMinCycleLength = 4;

// Quarter-block linear crossfade at each splice point. Cyclic mode has no
// exposed width control on any machine (that only appears in Intelligent
// mode -- plan "2.2"), so this is a fixed, conservative choice rather than
// a tuned one. Revisit by ear per plan section 8.
int _crossfadeLength(int cycleLength) {
    int overlap = cycleLength / 4;
    return std::max(0, std::min(overlap, cycleLength - 1));
}

// -- INTELLIGENT mode (build order stage 7, plan "2.2") ---------------------
//
// The manual's own words: "the S1000 'intelligently' varies the
// interpolation rate according to the sample content." That's a SOLA
// (synchronous overlap-add) search: instead of splicing at a fixed cycle
// length, each new frame is read from near its nominal position, but the
// exact offset is chosen by cross-correlating against the tail of what's
// already been written, picking whichever offset lines the waveforms up
// best. `quality` controls how far that search looks ("the time that the
// S1000 spends determining cycle lengths" -- more search = slower, better
// alignment); `width` controls the crossfade region at each splice.
//
// Cycle length itself does NOT apply here (only in CYCLIC -- plan "2.2"),
// so this uses its own internal frame size rather than
// AkzStretchParams.cycleLengthSamples.

// ~30ms: enough waveform context for the correlation search to find a
// meaningful alignment, short enough to still track fast transients. Not
// user-set -- see the comment above.
int _intelligentFrameSize(double sampleRateHz) {
    return std::max(64, static_cast<int>(std::lround(0.03 * sampleRateHz)));
}

// width 0..99 -> crossfade length as a fraction of the frame, 12.5%..75%.
// The upper end is capped well below 100% so the synthesis hop (frameSize
// - overlap, i.e. how much genuinely new material each iteration
// contributes) never collapses to near-zero.
int _intelligentOverlapLength(int width, int frameSize) {
    const int w = std::max(0, std::min(99, width));
    const double frac = 0.125 + 0.625 * (static_cast<double>(w) / 99.0);
    const int overlap = static_cast<int>(std::lround(frameSize * frac));
    return std::max(1, std::min(overlap, frameSize - 1));
}

// quality 0..99 -> how many samples either side of the nominal position
// the correlation search checks. 0 = no search at all (equivalent to a
// fixed-rate splice, fastest and lowest quality); 99 searches up to a
// quarter of the frame.
int _intelligentSearchRange(int quality, int frameSize) {
    const int q = std::max(0, std::min(99, quality));
    return static_cast<int>(std::lround((frameSize / 4.0) * (static_cast<double>(q) / 99.0)));
}

// Plain dot product, not a normalised correlation -- cheap, and avoids a
// divide-by-zero guard for silent regions that a normalised measure would
// need. Good enough to find phase alignment between two same-length
// windows, which is all this needs.
double _dotProduct(const float* a, const float* b, int len) {
    double sum = 0.0;
    for (int i = 0; i < len; ++i) {
        sum += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return sum;
}

// Every parameter the INTELLIGENT synthesis needs, derived once from
// (inLen, ratio, quality, width, sampleRateHz) so the actual synthesis
// and the length-only query (outputLength(), called before process() has
// ever run) can never compute a different answer from each other.
struct IntelligentPlan {
    bool useFallback = false; // inLen shorter than one frame -- see below
    int frameSize = 0;
    int overlapLen = 0;
    int synthesisHop = 0;     // frameSize - overlapLen: new material contributed per iteration
    double analysisHop = 0.0; // synthesisHop / ratio: nominal input advance per iteration
    int searchRange = 0;
    size_t numIterations = 0;
    size_t outputLength = 0;
};

IntelligentPlan _planIntelligent(size_t inLen, double ratio, int quality, int width, double sampleRateHz) {
    IntelligentPlan plan;
    plan.frameSize = _intelligentFrameSize(sampleRateHz);

    if (inLen == 0) {
        return plan;
    }
    if (inLen < static_cast<size_t>(plan.frameSize)) {
        // Too short for frame-based splice analysis to mean anything --
        // fall back to a plain resample. resample()'s ratio parameter is
        // "input advance per output sample," the inverse of the stretch
        // ratio used everywhere else in this file (stretch ratio > 1 =
        // longer output; resample ratio > 1 = shorter output), hence 1/ratio.
        plan.useFallback = true;
        plan.outputLength = resampledLength(inLen, 1.0 / ratio);
        return plan;
    }

    plan.overlapLen = _intelligentOverlapLength(width, plan.frameSize);
    plan.synthesisHop = std::max(1, plan.frameSize - plan.overlapLen);
    plan.analysisHop = static_cast<double>(plan.synthesisHop) / ratio;
    plan.searchRange = _intelligentSearchRange(quality, plan.frameSize);

    plan.numIterations = plan.analysisHop > 0.0
        ? static_cast<size_t>(std::floor((static_cast<double>(inLen) - plan.frameSize) / plan.analysisHop))
        : 0;
    plan.outputLength = static_cast<size_t>(plan.frameSize) + static_cast<size_t>(plan.synthesisHop) * plan.numIterations;
    return plan;
}

} // namespace

StretchEngine::StretchEngine(double sampleRateHz)
    : _sampleRateHz(sampleRateHz) {
    akz_stretch_params_default(AkzMachine_S950, &_params);
}

void StretchEngine::setParams(const AkzStretchParams& params) {
    _params = params;
    _dirty = true;
}

void StretchEngine::reset() {
    _source.clear();
    _output.clear();
    _readPos = 0;
    _dirty = true;
}

void StretchEngine::setSource(const float* sourceFrames, size_t frameCount) {
    _source.assign(sourceFrames, sourceFrames + frameCount);
    _readPos = 0;
    _dirty = true;
}

float StretchEngine::_sourceAt(long long index) const {
    if (index < 0 || index >= static_cast<long long>(_quantizedSource.size())) {
        return 0.0f;
    }
    return _quantizedSource[static_cast<size_t>(index)];
}

void StretchEngine::_synthesizeCyclicBlocks(std::vector<float>& out, size_t numOutBlocks, int cycleLength) const {
    const size_t numInBlocks = std::max<size_t>(1, _quantizedSource.size() / static_cast<size_t>(cycleLength));
    const int overlap = _crossfadeLength(cycleLength);
    const size_t baseOffset = out.size();

    out.resize(baseOffset + numOutBlocks * static_cast<size_t>(cycleLength));

    for (size_t k = 0; k < numOutBlocks; ++k) {
        // Fixed interpolation rate mapping (plan "2.1", CYCLIC): output
        // block k reads from input block floor(k * numInBlocks / numOutBlocks).
        // This naturally repeats input blocks when numOutBlocks > numInBlocks
        // (lengthening) and skips them when numOutBlocks < numInBlocks
        // (shortening), at a constant rate across the whole sample.
        size_t inBlockIndex = (k * numInBlocks) / numOutBlocks;
        inBlockIndex = std::min(inBlockIndex, numInBlocks - 1);
        const long long srcStart = static_cast<long long>(inBlockIndex) * cycleLength;

        float* dst = out.data() + baseOffset + k * static_cast<size_t>(cycleLength);
        for (int i = 0; i < cycleLength; ++i) {
            dst[i] = _sourceAt(srcStart + i);
        }

        // Crossfade the tail of this block into the head of the next
        // block's material -- "crossfades are used to make the
        // insertions and deletions as seamless as possible" (plan "2.1").
        // This blends what was about to play naturally with what the
        // splice is about to substitute, rather than adding length.
        //
        // Both legs must be read on the SAME output timebase: the
        // crossfade fills output positions [cycleLength - overlap,
        // cycleLength) of this block, and block k+1 begins at output
        // position cycleLength reading from srcStartNext. So leg `b` --
        // what the incoming block would have played at those same output
        // positions -- is srcStartNext - overlap + i, not srcStartNext + i.
        // The latter reads `overlap` samples further into the future than
        // leg `a`, so the two legs are never in phase with each other --
        // most audibly when srcStartNext == srcStart + cycleLength (blocks
        // already contiguous, e.g. always at timeFactorPercent == 100),
        // where there is no splice to hide at all and this used to comb-
        // filter the last quarter of every block regardless.
        if (k + 1 < numOutBlocks && overlap > 0) {
            size_t nextInBlockIndex = ((k + 1) * numInBlocks) / numOutBlocks;
            nextInBlockIndex = std::min(nextInBlockIndex, numInBlocks - 1);
            const long long srcStartNext = static_cast<long long>(nextInBlockIndex) * cycleLength;

            for (int i = 0; i < overlap; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(overlap);
                const float a = _sourceAt(srcStart + cycleLength - overlap + i);  // natural continuation
                const float b = _sourceAt(srcStartNext - overlap + i);            // incoming block, same output timebase as `a`
                dst[cycleLength - overlap + i] = a * (1.0f - t) + b * t;
            }
        }
    }
}

void StretchEngine::_synthesizeIntelligent(std::vector<float>& out, double ratio, int quality, int width) const {
    const size_t inLen = _quantizedSource.size();
    const IntelligentPlan plan = _planIntelligent(inLen, ratio, quality, width, _sampleRateHz);

    if (inLen == 0) {
        return;
    }

    if (plan.useFallback) {
        auto fallback = resample(_quantizedSource.data(), inLen, 1.0 / ratio, InterpolatorKind::Linear);
        out.insert(out.end(), fallback.begin(), fallback.end());
        return;
    }

    // First frame is copied verbatim -- there's no existing output tail
    // yet to align it against.
    out.insert(out.end(), _quantizedSource.begin(), _quantizedSource.begin() + plan.frameSize);

    double analysisPos = plan.analysisHop;
    for (size_t iter = 0; iter < plan.numIterations; ++iter, analysisPos += plan.analysisHop) {
        const long long nominalPos = static_cast<long long>(std::llround(analysisPos));

        // Search for the offset whose overlap region best matches the
        // tail already written -- this IS "intelligently varies the
        // interpolation rate according to the sample content" (plan
        // "2.2"): the effective read rate through the source drifts
        // slightly, iteration to iteration, to keep splices phase-aligned.
        long long bestOffset = 0;
        double bestScore = -1.0; // dot products of real audio are never below this for a non-trivial overlap
        const float* outTail = out.data() + out.size() - plan.overlapLen;
        for (long long delta = -plan.searchRange; delta <= plan.searchRange; ++delta) {
            const long long candidate = nominalPos + delta;
            if (candidate < 0 || static_cast<size_t>(candidate) + plan.overlapLen > inLen) {
                continue;
            }
            const double score = _dotProduct(outTail, _quantizedSource.data() + candidate, plan.overlapLen);
            if (score > bestScore) {
                bestScore = score;
                bestOffset = delta;
            }
        }

        long long framePos = nominalPos + bestOffset;
        framePos = std::max<long long>(0, std::min(framePos, static_cast<long long>(inLen) - plan.frameSize));
        const size_t framePosU = static_cast<size_t>(framePos);

        // Overlap-add the new frame's head into the tail already written...
        const size_t tailStart = out.size() - plan.overlapLen;
        for (int i = 0; i < plan.overlapLen; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(plan.overlapLen);
            out[tailStart + static_cast<size_t>(i)] =
                out[tailStart + static_cast<size_t>(i)] * (1.0f - t) + _quantizedSource[framePosU + static_cast<size_t>(i)] * t;
        }
        // ...then append the rest of the frame as genuinely new material.
        out.insert(out.end(),
                   _quantizedSource.begin() + static_cast<long long>(framePosU + static_cast<size_t>(plan.overlapLen)),
                   _quantizedSource.begin() + static_cast<long long>(framePosU + static_cast<size_t>(plan.frameSize)));
    }
}

void StretchEngine::_recompute() {
    _output.clear();
    _readPos = 0;

    // Record path (v2 heritage-roster plan, stage 4 -- rate/bandwidth
    // front end; build order stage 6's converter, folded into it): fresh
    // from the untouched _source every time, to the CURRENT machine's
    // sample rate AND bit depth -- this models "what this file would
    // sound like sampled into the machine" (this app's whole premise),
    // not a one-way bit-crush/decimate baked in permanently.
    // resolveSampleRateHz's 0-means-host-rate sentinel is what keeps
    // this a no-op for every preset/default that predates sampleRateHz
    // -- see RateModel.h. See ConverterModel.h for what's still not
    // modelled (companding, DAC low-level distortion -- stage 7).
    _quantizedSource = _source;
    const double effectiveRateHz = resolveSampleRateHz(_params.machine, _params.sampleRateHz, _sampleRateHz);
    applyRecordPath(_quantizedSource.data(), _quantizedSource.size(), _params.machine, effectiveRateHz, _sampleRateHz);

    const int cycleLength = std::max(kMinCycleLength, _params.cycleLengthSamples);
    // Defense in depth, not just a UI gate: a machine whose profile says
    // it has no time-stretch capability (S900 today; more join it as the
    // heritage roster grows) gets ratio 1.0 from the engine itself,
    // regardless of what timeFactorPercent the caller happens to be
    // holding. ContentView.swift's UI already disables the Stretch knob
    // for these machines, but that's advisory -- this is what actually
    // prevents it, the same "belt and suspenders" reasoning as the
    // maxStretchPercent clamp that fixed the S900 crash (see README's
    // "Gotchas fixed along the way").
    const double ratio = machineProfile(_params.machine).supportsTimeStretch
        ? static_cast<double>(_params.timeFactorPercent) / 100.0
        : 1.0;
    const double rawOutLen = static_cast<double>(_quantizedSource.size()) * ratio;

    if (_quantizedSource.empty()) {
        _dirty = false;
        return;
    }

    // S950 has no mode switch at all -- Mon1/Pol2 instead of CYCLIC/
    // INTELLIGENT (plan section 3.2, MachineProfile.hasModeSwitch) -- so
    // its params.mode is ignored rather than trusted, matching the
    // AkzStretchParams.mode field's own doc comment in AkaizerCore.h.
    const AkzStretchMode effectiveMode = machineProfile(_params.machine).hasModeSwitch
        ? _params.mode
        : AkzStretchMode_Cyclic;

    if (effectiveMode == AkzStretchMode_Intelligent) {
        _synthesizeIntelligent(_output, ratio, _params.quality, _params.width);
    } else {
        // CLASSIC: quantise to a whole number of cycle-length blocks and
        // stop there. The rounding error against the requested length IS
        // the documented "imprecise timing" -- do not compensate for it.
        const size_t numOutBlocksClassic = std::max<size_t>(1, static_cast<size_t>(std::llround(rawOutLen / cycleLength)));
        _synthesizeCyclicBlocks(_output, numOutBlocksClassic, cycleLength);
    }

    if (_params.engine == AkzEngine_Revised) {
        // REVISED: the same synthesis (whichever mode produced it), then
        // linearly resampled to the exact requested length. That
        // resample is small by construction for CYCLIC (it only corrects
        // the sub-block remainder) and is exactly what drifts the pitch
        // slightly while landing on perfect timing -- plan "2.3". For
        // INTELLIGENT the same correction applies on top of SOLA's own
        // (similarly approximate) natural length.
        const size_t desiredLen = std::max<size_t>(1, static_cast<size_t>(std::llround(rawOutLen)));
        const size_t synthLen = _output.size();

        if (synthLen != desiredLen && synthLen > 1) {
            std::vector<float> resampled(desiredLen);
            const double step = static_cast<double>(synthLen - 1) / static_cast<double>(std::max<size_t>(1, desiredLen - 1));
            for (size_t n = 0; n < desiredLen; ++n) {
                const double srcPos = static_cast<double>(n) * step;
                const size_t i0 = static_cast<size_t>(srcPos);
                const size_t i1 = std::min(i0 + 1, synthLen - 1);
                const float frac = static_cast<float>(srcPos - static_cast<double>(i0));
                resampled[n] = _output[i0] * (1.0f - frac) + _output[i1] * frac;
            }
            _output = std::move(resampled);
        } else if (synthLen != desiredLen) {
            _output.resize(desiredLen, 0.0f);
        }
    }

    // Transpose (build order stage 5): varispeed applied AFTER the
    // stretch, per the signal chain in the project plan ("4. Signal
    // chain"). A no-op at 0 semitones -- semitonesToRatio(0) == 1.0, and
    // resample() at ratio 1.0 reproduces the input length exactly, so
    // this never perturbs any of the length arithmetic above when
    // transpose isn't in use.
    const double transposeRatio = semitonesToRatio(_params.transposeSemitones);
    if (_params.transposeSemitones != 0.0f && !_output.empty()) {
        const InterpolatorKind kind = interpolatorKindForMachine(_params.machine);
        _output = resample(_output.data(), _output.size(), transposeRatio, kind);
    }

    // Cached here, before the filter runs, so reapplyFilterOnly() can redo
    // just the last stage when a live-audition change turns out to be
    // filter-only (see paramsDifferOnlyInFilter()) instead of re-running
    // everything above.
    _preFilter = _output;

    // Filter (build order stage 6): last stage in the signal chain, same
    // as the real hardware's VCF running after pitch interpolation. Cutoff
    // tracks transposeRatio only on machines whose filter genuinely does
    // (S900/S950 -- see FilterModel.h); harmless to always pass it.
    if (!_output.empty()) {
        applyFilter(_output.data(), _output.size(), _params.machine, _params.filterCutoff01, _params.filterResonance01, _sampleRateHz, transposeRatio);
    }

    _dirty = false;
}

void StretchEngine::reapplyFilterOnly(const AkzStretchParams& params) {
    // transposeRatio must match what _recompute() would have passed for
    // pitch-tracking machines (S900/S950 -- FilterModel.h); transpose
    // itself is not a filter-only field, so if it's part of what changed
    // the caller should not have taken this path at all.
    const double transposeRatio = semitonesToRatio(params.transposeSemitones);
    _output = _preFilter;
    if (!_output.empty()) {
        applyFilter(_output.data(), _output.size(), params.machine, params.filterCutoff01, params.filterResonance01, _sampleRateHz, transposeRatio);
    }
    _params = params;
    _readPos = 0; // internal extraction cursor for process() -- see the identical reset in _recompute(); unrelated to any playback position a caller tracks externally
    _dirty = false;
}

// Guard rails for the memcmp below, both checked at compile time rather
// than trusted:
//
//   1. standard-layout is what makes memcmp over this type well-defined
//      in the first place (no vtable, no base classes, nothing the
//      language is free to lay out however it likes).
//   2. The explicit per-field size sum catches padding: if it ever falls
//      short of sizeof(AkzStretchParams), either a new field was added
//      to the struct but NOT to this list (the exact "forgotten mirror"
//      this is meant to catch), or a field's size introduced an
//      alignment gap the memcmp would read as uninitialised bytes. Every
//      field today is 4 bytes (enums, int32, float) specifically so the
//      struct stays gap-free; a differently-sized field needs this
//      approach revisited, not just extended.
//   3. The literal size check is a blunter trip-wire: any struct-size
//      change at all, for any reason, must not go unnoticed. Bump N,
//      then update the four mirrors named below in the same commit.
static_assert(std::is_standard_layout<AkzStretchParams>::value,
    "AkzStretchParams must stay standard-layout for paramsDifferOnlyInFilter's memcmp to be well-defined.");
static_assert(
    sizeof(AkzStretchParams::machine) + sizeof(AkzStretchParams::engine) + sizeof(AkzStretchParams::mode) +
    sizeof(AkzStretchParams::timeFactorPercent) + sizeof(AkzStretchParams::cycleLengthSamples) +
    sizeof(AkzStretchParams::quality) + sizeof(AkzStretchParams::width) +
    sizeof(AkzStretchParams::transposeSemitones) +
    sizeof(AkzStretchParams::filterCutoff01) + sizeof(AkzStretchParams::filterResonance01) +
    sizeof(AkzStretchParams::sampleRateHz)
    == sizeof(AkzStretchParams),
    "AkzStretchParams has a field this sum doesn't account for (or padding), so paramsDifferOnlyInFilter's "
    "memcmp would compare uninitialised bytes. If you added a field: add it to this sum AND to the "
    "sizeof(AkzStretchParams) == N check below.");
static_assert(sizeof(AkzStretchParams) == 44,
    "AkzStretchParams changed size -- update, in the same commit: akz_stretch_params_default (below), "
    "the per-field sum static_assert above, StretchBridge.swift's positional AkzStretchParams init "
    "(intentionally positional so this is a compile error there too), ParamSnapshot.swift, and "
    "PresetStore.swift's decodeIfPresent handling for the new field.");

bool StretchEngine::paramsDifferOnlyInFilter(const AkzStretchParams& a, const AkzStretchParams& b) {
    const bool filterFieldsDiffer = a.filterCutoff01 != b.filterCutoff01 || a.filterResonance01 != b.filterResonance01;
    if (!filterFieldsDiffer) {
        return false; // identical, or nothing filter-related changed -- caller has nothing to special-case
    }

    // Copy-and-zero-the-filter-fields-then-memcmp, rather than the old
    // hand-enumerated field-by-field comparison: every OTHER field is
    // automatically covered, including ones added after this was
    // written (sampleRateHz, and whatever stage 4/7 add), instead of
    // silently taking the filter-only cheap path for a change this
    // function was never updated to know about.
    AkzStretchParams aWithoutFilter = a;
    AkzStretchParams bWithoutFilter = b;
    aWithoutFilter.filterCutoff01 = 0.0f;
    aWithoutFilter.filterResonance01 = 0.0f;
    bWithoutFilter.filterCutoff01 = 0.0f;
    bWithoutFilter.filterResonance01 = 0.0f;
    return std::memcmp(&aWithoutFilter, &bWithoutFilter, sizeof(AkzStretchParams)) == 0;
}

size_t StretchEngine::process(float* outFrames, size_t maxOutFrames) {
    if (_dirty) {
        _recompute();
    }
    const size_t available = _output.size() - std::min(_readPos, _output.size());
    const size_t toWrite = std::min(available, maxOutFrames);
    if (toWrite > 0) {
        std::memcpy(outFrames, _output.data() + _readPos, toWrite * sizeof(float));
    }
    _readPos += toWrite;
    return toWrite;
}

size_t StretchEngine::outputLength() const {
    if (_dirty) {
        // outputLength() is documented as computable without a prior
        // process() call. Since the real computation lives in _recompute()
        // and that's non-const, mirror just the length arithmetic here
        // rather than mutating state from a const method.
        if (_source.empty()) {
            return 0;
        }
        const int cycleLength = std::max(kMinCycleLength, _params.cycleLengthSamples);
        // Mirrors _recompute()'s same defense-in-depth clamp -- see the
        // comment there. Must stay identical or outputLength() (callable
        // before process() has ever run) and the actual render disagree.
        const double ratio = machineProfile(_params.machine).supportsTimeStretch
            ? static_cast<double>(_params.timeFactorPercent) / 100.0
            : 1.0;
        const double rawOutLen = static_cast<double>(_source.size()) * ratio;
        const AkzStretchMode effectiveMode = machineProfile(_params.machine).hasModeSwitch
            ? _params.mode
            : AkzStretchMode_Cyclic;

        size_t stretchedLen;
        if (_params.engine == AkzEngine_Revised) {
            // REVISED always lands on the exact requested length,
            // regardless of which mode produced the intermediate
            // synthesis -- see _recompute()'s REVISED branch.
            stretchedLen = std::max<size_t>(1, static_cast<size_t>(std::llround(rawOutLen)));
        } else if (effectiveMode == AkzStretchMode_Intelligent) {
            stretchedLen = _planIntelligent(_source.size(), ratio, _params.quality, _params.width, _sampleRateHz).outputLength;
        } else {
            const size_t numOutBlocksClassic = std::max<size_t>(1, static_cast<size_t>(std::llround(rawOutLen / cycleLength)));
            stretchedLen = numOutBlocksClassic * static_cast<size_t>(cycleLength);
        }

        if (_params.transposeSemitones == 0.0f) {
            return stretchedLen;
        }
        return resampledLength(stretchedLen, semitonesToRatio(_params.transposeSemitones));
    }
    return _output.size();
}

} // namespace akz

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

struct AkzStretchEngine {
    akz::StretchEngine impl;
    explicit AkzStretchEngine(double sampleRateHz) : impl(sampleRateHz) {}
};

void akz_stretch_params_default(AkzMachine machine, AkzStretchParams* out_params) {
    if (!out_params) return;
    const AkzMachineProfile& profile = akz::machineProfile(machine);

    out_params->machine = machine;
    out_params->engine = AkzEngine_Classic;
    out_params->mode = AkzStretchMode_Cyclic;
    out_params->timeFactorPercent = 100.0f;
    out_params->cycleLengthSamples = profile.defaultCycleLength > 0 ? profile.defaultCycleLength : 1000;
    out_params->quality = 10;
    out_params->width = 10;
    out_params->transposeSemitones = 0.0f;
    out_params->filterCutoff01 = 1.0f;   // fully open -- matches the hardware's own "0xffff = Nyquist" default
    out_params->filterResonance01 = 0.0f;
    out_params->sampleRateHz = 0.0f;     // 0 = machine default -- see AkaizerCore.h
}

AkzStretchEngine* akz_stretch_engine_create(double sampleRateHz) {
    return new AkzStretchEngine(sampleRateHz);
}

void akz_stretch_engine_destroy(AkzStretchEngine* engine) {
    delete engine;
}

void akz_stretch_engine_set_params(AkzStretchEngine* engine, const AkzStretchParams* params) {
    if (!engine || !params) return;
    engine->impl.setParams(*params);
}

void akz_stretch_engine_reset(AkzStretchEngine* engine) {
    if (!engine) return;
    engine->impl.reset();
}

void akz_stretch_engine_set_source(AkzStretchEngine* engine, const float* source_frames, size_t frame_count) {
    if (!engine) return;
    engine->impl.setSource(source_frames, frame_count);
}

size_t akz_stretch_engine_process(AkzStretchEngine* engine, float* out_frames, size_t max_out_frames) {
    if (!engine) return 0;
    return engine->impl.process(out_frames, max_out_frames);
}

size_t akz_stretch_engine_output_length(const AkzStretchEngine* engine) {
    if (!engine) return 0;
    return engine->impl.outputLength();
}
