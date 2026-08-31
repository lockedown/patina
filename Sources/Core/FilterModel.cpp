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

// Instantiates one stage of `topology`. `poles` is only meaningful for
// OnePoleCascade (each other topology's pole count is a property of the
// real chip, not a per-call parameter); `resonanceCode` is only
// meaningful for the resonant topologies. Passing the irrelevant
// argument for a given topology is harmless -- the constructor that
// ignores it just ignores it.
std::unique_ptr<IFilterStage> makeFilterStage(AkzFilterTopology topology, double cutoffHz, int resonanceCode, double sampleRateHz, int poles) {
    switch (topology) {
        case AkzFilterTopology_ChamberlinSvf:
            return std::make_unique<ChamberlinSVF>(cutoffHz, resonanceCode, sampleRateHz);
        case AkzFilterTopology_OnePoleCascade:
        // TptSvf/SsmLadder/CemStateVariable/SwitchedCapacitor land with
        // the machines that need them (heritage-roster plan stages 6,
        // 10) -- falling through to OnePoleCascade rather than silently
        // misrendering is a deliberate placeholder, not a real choice
        // for any of those topologies' actual character.
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
        stages.push_back(makeFilterStage(profile.filterTopology, cutoffHz, resonanceCode, sampleRateHz, poles));
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
