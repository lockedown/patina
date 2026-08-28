// FilterModel.cpp
//
// See FilterModel.h for the per-machine rationale.

#include "FilterModel.h"
#include "MachineProfile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace akz {

namespace {

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
class OnePoleLowpassCascade {
public:
    OnePoleLowpassCascade(int poles, double cutoffHz, double sampleRateHz)
        : _poles(std::max(1, poles)) {
        const double clampedCutoff = std::min(cutoffHz, sampleRateHz * 0.49);
        _a = 1.0 - std::exp(-2.0 * M_PI * clampedCutoff / sampleRateHz);
        _state.assign(static_cast<size_t>(_poles), 0.0);
    }

    float process(float x) {
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
class ChamberlinSVF {
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

    float process(float x) {
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

} // namespace

void applyFilter(float* buffer, size_t count, AkzMachine machine, float cutoff01, float resonance01, double sampleRateHz, double transposeRatio) {
    if (count == 0) return;

    const AkzMachineProfile& profile = machineProfile(machine);
    const double trackedRatio = profile.filterTracksPitch ? transposeRatio : 1.0;
    const double cutoffHz = _mapCutoff01ToHz(cutoff01, sampleRateHz) * trackedRatio;

    if (profile.filterHasResonance) {
        const int resonanceCode = static_cast<int>(std::lround(std::max(0.0f, std::min(1.0f, resonance01)) * 15.0f));
        ChamberlinSVF stage1(cutoffHz, resonanceCode, sampleRateHz);
        // S3200's optional second digital filter, both stages set to
        // lowpass in series -> 24 dB/oct "Moog-ish" mode (plan section
        // 3.4). Only S3200 has filterSlopeDbPerOctave == 24 among the
        // resonant machines.
        const bool hasSecondStage = profile.filterSlopeDbPerOctave >= 24.0;
        if (hasSecondStage) {
            ChamberlinSVF stage2(cutoffHz, resonanceCode, sampleRateHz);
            for (size_t i = 0; i < count; ++i) {
                buffer[i] = stage2.process(stage1.process(buffer[i]));
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                buffer[i] = stage1.process(buffer[i]);
            }
        }
    } else {
        const int poles = std::max(1, static_cast<int>(std::lround(profile.filterSlopeDbPerOctave / 6.0)));
        OnePoleLowpassCascade cascade(poles, cutoffHz, sampleRateHz);
        for (size_t i = 0; i < count; ++i) {
            buffer[i] = cascade.process(buffer[i]);
        }
    }
}

} // namespace akz
