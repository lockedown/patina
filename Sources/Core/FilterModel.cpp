// FilterModel.cpp
//
// See FilterModel.h for the per-machine rationale.

#include "FilterModel.h"
#include "MachineProfile.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace akz {

namespace {

// Common interface every filter topology implements, so applyFilter()
// below can run N stages of whatever topology the profile names without
// knowing which concrete class it got -- the factory (makeFilterStage,
// below the two classes) is what turns AkzFilterTopology into one of
// these. Adding a topology (SsmLadder, CemStateVariable,
// SwitchedCapacitor -- see AkaizerCore.h) means one new class and one
// new factory case, never a change to applyFilter's dispatch itself.
class IFilterStage {
public:
    virtual ~IFilterStage() = default;
    virtual float process(float x) = 0;
};

// Logarithmic 20 Hz..Nyquist mapping so the cutoff control feels even
// across its range (matches how a synth filter knob is normally scaled,
// and gives sensible behaviour at cutoff01 == 1.0 -> Nyquist, the
// hardware's own "fully open" convention -- "0xffff = Nyquist").
double _mapCutoff01ToHz(float cutoff01, double sampleRateHz) {
    const double nyquist = sampleRateHz / 2.0;
    const double c = std::max(0.0f, std::min(1.0f, cutoff01));
    return 20.0 * std::pow(nyquist / 20.0, c);
}

// A cascade of identical one-pole lowpass stages -- the simple,
// real-time-safe stand-in for the S900/S950 analog (36 dB/oct) and
// S1000 digital (18 dB/oct) filters. This is NOT a precision Butterworth
// design (a true Nth-order Butterworth needs distinct Q per biquad
// stage, not N identical one-poles) -- it's a reasonably-shaped
// multi-pole rolloff, which is honestly about as much precision as the
// citation supports anyway: the S950's real filter is documented as
// departing from an ideal Butterworth response at high bandwidth (droop,
// clock feedthrough -- plan section 3.3), so chasing textbook-exact
// Butterworth coefficients here would be precision spent modelling a
// shape the real hardware didn't actually have either.
class OnePoleLowpassCascade : public IFilterStage {
public:
    OnePoleLowpassCascade(int poles, double cutoffHz, double sampleRateHz)
        : _poles(std::max(1, poles)) {
        const double clampedCutoff = std::min(cutoffHz, sampleRateHz * 0.49);
        _a = 1.0 - std::exp(-2.0 * M_PI * clampedCutoff / sampleRateHz);
        _state.assign(static_cast<size_t>(_poles), 0.0);
    }

    float process(float x) override {
        double v = static_cast<double>(x);
        for (int i = 0; i < _poles; ++i) {
            _state[static_cast<size_t>(i)] += _a * (v - _state[static_cast<size_t>(i)]);
            v = _state[static_cast<size_t>(i)];
        }
        return static_cast<float>(v);
    }

private:
    int _poles;
    double _a;
    std::vector<double> _state;
};

// The exact difference equation from the reverse-engineered
// `l7a1045_l6028_dsp_a.cpp` (S2000/S3000/S3200 voice chip) -- see
// FilterModel.h. Implemented against the code, not its comment: damping
// bottoms out at 1/16, so resonanceCode 15 approaches but never reaches
// self-oscillation.
class ChamberlinSVF : public IFilterStage {
public:
    ChamberlinSVF(double cutoffHz, int resonanceCode, double sampleRateHz) {
        // Stability note, corrected: the naive (non-zero-delay-feedback)
        // Chamberlin SVF is nowhere near stable all the way to k=2. An
        // empirical sweep (all 16 resonance codes, 200k samples) found
        // the actual boundary at k ~= 1.23 -- clamping to 1.9 as
        // originally written let this genuinely diverge to +-inf within
        // a few hundred samples on a real render (caught by
        // IntelligentModeTests.cpp's shortening test, which runs long
        // enough for the divergence to show; FilterModelTests.cpp's
        // shorter windows and non-default cutoffs happened not to
        // exercise this). 1.1 leaves comfortable margin below the
        // measured boundary across every resonance setting.
        //
        // Practical consequence: this SVF cannot reach the full 20
        // Hz..Nyquist range the cutoff control implies -- k=1.1 caps the
        // real achievable cutoff around 8 kHz at 44.1kHz sample rate.
        // The real fixed-point hardware presumably has its own way of
        // staying stable at higher cutoffs (or simply doesn't hit this
        // failure mode in fixed-point the way float divergence does);
        // this is this project's simplification, not a property of the
        // reverse-engineered algorithm itself.
        const double kRaw = 2.0 * std::sin(M_PI * std::min(cutoffHz, sampleRateHz * 0.49) / sampleRateHz);
        _k = std::min(kRaw, 1.1);
        const int clampedRes = std::max(0, std::min(15, resonanceCode));
        _damping = 1.0 - static_cast<double>(clampedRes) / 16.0; // bottoms out at 1/16, never 0
    }

    float process(float x) override {
        const double h = static_cast<double>(x) - _low - _damping * _band;
        _band += _k * h;
        _low += _k * _band;

        // Hard safety backstop, independent of the k clamp above: no
        // input to this filter should ever be able to make it diverge to
        // +-inf/NaN. If some combination this project hasn't swept
        // manages to destabilise it anyway, clamp state rather than let
        // non-finite values escape into the rest of the pipeline (and,
        // for a real-time render, into CoreAudio).
        constexpr double kStateLimit = 100.0;
        _band = std::max(-kStateLimit, std::min(kStateLimit, _band));
        _low = std::max(-kStateLimit, std::min(kStateLimit, _low));

        return static_cast<float>(_low);
    }

private:
    double _k = 0.0;
    double _damping = 1.0;
    double _low = 0.0;
    double _band = 0.0;
};

// Zero-delay-feedback ("topology-preserving transform") state-variable
// filter -- Zavalishin, "The Art of VA Filter Design". Unconditionally
// stable for any g >= 0, k >= 0: no analogue of ChamberlinSVF's k <= 2
// instability exists here, and none of ChamberlinSVF's empirical k <=
// 1.1 clamp is needed. This is what migrates S2000/S3000/S3200 off
// ChamberlinSvf (v2 heritage-roster plan's "TPT SVF" stage, a
// deliberate, accepted sonic break) -- see ChamberlinSVF's own comment
// above for the ~8kHz cutoff ceiling and passband-gain clipping bug
// this replaces.
class TptSvf : public IFilterStage {
public:
    TptSvf(double cutoffHz, int resonanceCode, double sampleRateHz, double resonanceCompensation01) {
        const double clampedCutoff = std::min(cutoffHz, sampleRateHz * 0.49);
        const double g = std::tan(M_PI * clampedCutoff / sampleRateHz);

        // Same damping curve as ChamberlinSVF (bottoms out at 1/16,
        // never reaching true self-oscillation) mapped to TPT's
        // k = 1/Q -- k = 2 * damping is this project's own curve [I],
        // not a hardware citation: damping = 1 (resonanceCode 0) gives
        // k = 2 (Q = 0.5, an over-damped "no resonance" starting
        // point); damping = 1/16 (resonanceCode 15) gives k = 0.125
        // (Q = 8, a strong but finite, unconditionally stable peak).
        const int clampedRes = std::max(0, std::min(15, resonanceCode));
        const double damping = 1.0 - static_cast<double>(clampedRes) / 16.0;
        _k = 2.0 * damping;

        _a1 = 1.0 / (1.0 + g * (g + _k));
        _a2 = g * _a1;
        _a3 = g * _a2;

        // Passband-gain compensation (the actual fix for the clipping
        // bug ChamberlinSVF's clamp left behind): the resonant peak's
        // height is approximately Q = 1/k -- real hardware character,
        // kept, not eliminated -- but uncompensated it can exceed 1.0
        // and clip downstream through PCMConversion.matchedGain's A/B
        // loudness match. resonanceCompensation01 (AkzMachineProfile,
        // [I] -- no manual specifies this) interpolates between 0 (no
        // compensation, the peak passes through at full height) and 1
        // (full compensation, output peak held at unity even at
        // maximum resonance).
        const double peakGain = std::max(1.0, 1.0 / std::max(_k, 1e-6));
        _inputScale = 1.0 / (1.0 + resonanceCompensation01 * (peakGain - 1.0));
    }

    float process(float x) override {
        const double v0 = static_cast<double>(x) * _inputScale;
        const double v3 = v0 - _ic2eq;
        const double v1 = _a1 * _ic1eq + _a2 * v3;
        const double v2 = _ic2eq + _a2 * _ic1eq + _a3 * v3;
        _ic1eq = 2.0 * v1 - _ic1eq;
        _ic2eq = 2.0 * v2 - _ic2eq;
        return static_cast<float>(v2); // lowpass output, same choice as ChamberlinSVF
    }

private:
    double _k = 2.0;
    double _a1 = 0.0, _a2 = 0.0, _a3 = 0.0;
    double _inputScale = 1.0;
    double _ic1eq = 0.0, _ic2eq = 0.0;
};

// A simplified digital model of a 4-pole transistor ladder filter
// (Stilson & Smith, "Analyzing the Moog VCF with Considerations for
// Digital Implementation," 1996) -- stands in for the SSM2044/SSM2045-
// class ladder filters the heritage-roster research found on the
// Emulator II (SSM2045, one per channel, manual-confirmed "4 pole
// lowpass filter") and, optionally and NOT modelled here, the SP-1200
// (SSM2044, a colour option on channels 1-2 only, not the main signal
// path -- see MachineProfile.cpp). Four identical one-pole stages
// inside a resonance feedback loop -- distinct in character from the
// SVF topologies: capable of genuine self-oscillation, which for a
// real transistor ladder is a desired, bounded character rather than
// the unintentional divergence ChamberlinSVF's clamp existed to
// prevent. The feedback path is soft-clipped (tanh) rather than left
// linear specifically so the loop stays bounded at high resonance --
// this is both a stability requirement (a linear ladder model WOULD
// diverge past its own resonance boundary, the exact failure class
// ChamberlinSVF hit) and a real modelling technique: a transistor
// ladder's own saturation is what keeps its self-oscillation amplitude
// bounded in the first place, not a separate limiter bolted on.
class SsmLadder : public IFilterStage {
public:
    SsmLadder(double cutoffHz, int resonanceCode, double sampleRateHz, double resonanceCompensation01) {
        const double clampedCutoff = std::min(cutoffHz, sampleRateHz * 0.49);
        _g = 1.0 - std::exp(-2.0 * M_PI * clampedCutoff / sampleRateHz);

        // 0..15 -> 0..4.0 feedback amount -- this project's own curve
        // [I], same status as ChamberlinSVF/TptSvf's damping mappings.
        // 4.0 is the classic Moog self-oscillation boundary for 4
        // identical one-pole stages (each contributes ~-90 degrees at
        // cutoff; 4 poles = -360 degrees, satisfying the Barkhausen
        // criterion at unity feedback gain).
        const int clampedRes = std::max(0, std::min(15, resonanceCode));
        _resonanceAmount = 4.0 * (static_cast<double>(clampedRes) / 15.0);

        // Passband-gain compensation, same rationale/formula as TptSvf
        // -- peakGain approximated from the resonance amount rather
        // than derived analytically (the tanh nonlinearity makes an
        // exact closed form impractical); [I].
        const double peakGain = std::max(1.0, 1.0 + _resonanceAmount * 0.9);
        _inputScale = 1.0 / (1.0 + resonanceCompensation01 * (peakGain - 1.0));
    }

    float process(float x) override {
        const double feedback = std::tanh(_resonanceAmount * _stage[3]);
        double v = static_cast<double>(x) * _inputScale - feedback;
        for (int i = 0; i < 4; ++i) {
            _stage[i] += _g * (v - _stage[i]);
            v = _stage[i];
        }

        // Hard safety backstop, same discipline as ChamberlinSVF's --
        // no input should ever make this filter emit non-finite values,
        // regardless of how thoroughly the tanh soft-clip above has
        // been swept.
        constexpr double kStateLimit = 100.0;
        for (double& s : _stage) {
            s = std::max(-kStateLimit, std::min(kStateLimit, s));
        }

        return static_cast<float>(_stage[3]);
    }

private:
    double _g = 0.0;
    double _resonanceAmount = 0.0;
    double _inputScale = 1.0;
    double _stage[4] = {0.0, 0.0, 0.0, 0.0};
};

// Instantiates one stage of `topology`. `poles` is only meaningful for
// OnePoleCascade (each other topology's pole count is a property of the
// real chip, not a per-call parameter); `resonanceCode`/
// `resonanceCompensation01` are only meaningful for the resonant
// topologies. Passing an irrelevant argument for a given topology is
// harmless -- the constructor that ignores it just ignores it.
std::unique_ptr<IFilterStage> makeFilterStage(AkzFilterTopology topology, double cutoffHz, int resonanceCode, double sampleRateHz, int poles, double resonanceCompensation01) {
    switch (topology) {
        case AkzFilterTopology_ChamberlinSvf:
            return std::make_unique<ChamberlinSVF>(cutoffHz, resonanceCode, sampleRateHz);
        case AkzFilterTopology_TptSvf:
        // CemStateVariable (Fairlight's CEM3320/SSM2045-era VCF, Mirage's
        // CEM3328) is modelled with the same TptSvf math, not a separate
        // class -- both real chips are commonly modelled as resonant
        // state-variable filters, and this project already has one that
        // is unconditionally stable. filterStageCount == 2 (two 2-pole
        // stages in series, same technique as S3200's 24dB/oct mode) is
        // how CEM3328's cited 4-pole/24dB spec is reached -- see
        // MachineProfile.cpp.
        case AkzFilterTopology_CemStateVariable:
            return std::make_unique<TptSvf>(cutoffHz, resonanceCode, sampleRateHz, resonanceCompensation01);
        case AkzFilterTopology_SsmLadder:
            return std::make_unique<SsmLadder>(cutoffHz, resonanceCode, sampleRateHz, resonanceCompensation01);
        case AkzFilterTopology_OnePoleCascade:
        // SwitchedCapacitor lands if a machine ever needs it modelled as
        // its OWN output-filter topology (Fairlight's citation is for an
        // INPUT anti-alias stage, already covered by RateModel's
        // aaFilterCutoffRatio/aaFilterPoles, not this factory) --
        // falling through to OnePoleCascade rather than silently
        // misrendering is a deliberate placeholder, not a real choice
        // for its actual character.
        default:
            return std::make_unique<OnePoleLowpassCascade>(poles, cutoffHz, sampleRateHz);
    }
}

} // namespace

void applyFilter(float* buffer, size_t count, AkzMachine machine, float cutoff01, float resonance01, double sampleRateHz, double transposeRatio) {
    if (count == 0) return;

    const AkzMachineProfile& profile = machineProfile(machine);
    const double trackedRatio = profile.filterTracksPitch ? transposeRatio : 1.0;
    const double cutoffHz = _mapCutoff01ToHz(cutoff01, sampleRateHz) * trackedRatio;
    const int resonanceCode = static_cast<int>(std::lround(std::max(0.0f, std::min(1.0f, resonance01)) * 15.0f));
    const int poles = std::max(1, static_cast<int>(std::lround(profile.filterSlopeDbPerOctave / 6.0)));

    // filterStageCount replaces the old ">= 24.0 dB/oct" heuristic for
    // S3200's second series SVF stage -- see AkaizerCore.h. Generalises
    // for free to any future machine needing N stages of the same
    // topology in series, not just "one or two."
    const int stageCount = std::max(1, profile.filterStageCount);
    std::vector<std::unique_ptr<IFilterStage>> stages;
    stages.reserve(static_cast<size_t>(stageCount));
    for (int i = 0; i < stageCount; ++i) {
        stages.push_back(makeFilterStage(profile.filterTopology, cutoffHz, resonanceCode, sampleRateHz, poles, profile.filterResonanceCompensation01));
    }

    for (size_t i = 0; i < count; ++i) {
        float v = buffer[i];
        for (auto& stage : stages) {
            v = stage->process(v);
        }
        buffer[i] = v;
    }
}

} // namespace akz
