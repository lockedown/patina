// FilterStages.h
//
// The IFilterStage hierarchy that used to live entirely inside
// FilterModel.cpp's anonymous namespace. Hoisted here, unchanged in
// their per-sample math, so a second call site can hold one of these
// objects alive ACROSS calls instead of FilterModel.cpp's own
// construct-run-discard-per-render usage (applyFilter() below is still
// exactly that: it builds a fresh stack of stages every call, same as
// before this file existed).
//
// The addition every class gets here is retune(cutoffHz, resonanceCode):
// recompute coefficients from a new cutoff/resonance WITHOUT touching
// _state/_low/_band/_ic1eq/_ic2eq/_stage -- the whole point is letting a
// live cutoff/resonance knob move without the filter memory (and thus
// the audio) glitching, the way constructing a brand new stage every
// render (applyFilter's own usage) necessarily would. See
// Sources/Core/RealtimeChannel.h for the real-time caller.
//
// sampleRateHz, poles and resonanceCompensation01 are treated as
// construction-time constants (machine/sample-rate properties, not
// something a live knob changes), so retune() only ever takes the two
// fields an actual cutoff/resonance control can move.

#ifndef AKAIZER_FILTER_STAGES_H
#define AKAIZER_FILTER_STAGES_H

#include "include/AkaizerCore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace akz {

// See FilterModel.cpp's original header comment (same class, same
// dispatch role) for why this interface exists.
class IFilterStage {
public:
    virtual ~IFilterStage() = default;
    virtual float process(float x) = 0;

    // Block-level counterpart of process() -- one virtual call per
    // buffer instead of one per sample, so each concrete stage can keep
    // its state in registers across the loop (the per-sample override
    // can't: the call boundary forces state back to memory every
    // sample). Default loops process(); every stage below overrides it
    // with the same math written as an internal loop -- bit-identical,
    // just faster. Callers processing whole buffers (applyFilter,
    // RealtimeChannel's MachineChain) should always use this.
    virtual void processBlock(float* buf, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            buf[i] = process(buf[i]);
        }
    }

    virtual void retune(double cutoffHz, int resonanceCode) = 0;

    // Zeroes internal filter memory WITHOUT touching coefficients --
    // the real-time counterpart of applyFilter()'s own "every filter
    // kind resets its internal state at the start of a render" (see
    // this file's original FilterModel.h header comment), for a
    // RealtimeChannel::reset() (transport stop, not an ordinary
    // cutoff/resonance move -- see RealtimeChannel.h).
    virtual void reset() = 0;
};

// Logarithmic 20 Hz..Nyquist mapping -- see FilterModel.cpp's original
// comment. Shared by applyFilter() and any real-time caller so the two
// paths agree on what a given cutoff01 means.
inline double filterStagesMapCutoff01ToHz(float cutoff01, double sampleRateHz) {
    const double nyquist = sampleRateHz / 2.0;
    const double c = std::max(0.0f, std::min(1.0f, cutoff01));
    return 20.0 * std::pow(nyquist / 20.0, c);
}

// Soft-knee limiter -- see FilterModel.cpp's original comment for the
// full rationale (resonance makeup-gain headroom).
constexpr double kFilterStagesSoftKneeThreshold = 0.98;
inline double filterStagesSoftKneeLimit(double x) {
    const double ax = std::fabs(x);
    if (ax <= kFilterStagesSoftKneeThreshold) return x;
    const double headroom = 1.0 - kFilterStagesSoftKneeThreshold;
    const double excess = ax - kFilterStagesSoftKneeThreshold;
    const double compressed = kFilterStagesSoftKneeThreshold + headroom * std::tanh(excess / headroom);
    return std::copysign(compressed, x);
}

// A cascade of identical one-pole lowpass stages -- S900/S950 analog
// (36 dB/oct) and S1000 digital (18 dB/oct). See FilterModel.cpp's
// original comment for the "not a precision Butterworth" caveat.
class OnePoleLowpassCascade : public IFilterStage {
public:
    // 8 covers every profile in the roster with headroom (36 dB/oct -> 6
    // poles is today's max); poles is clamped to it rather than trusted.
    static constexpr int kMaxPoles = 8;

    OnePoleLowpassCascade(int poles, double cutoffHz, double sampleRateHz)
        : _poles(std::max(1, std::min(kMaxPoles, poles))), _sampleRateHz(sampleRateHz) {
        _state.fill(0.0);
        retune(cutoffHz, 0);
    }

    float process(float x) override {
        double v = static_cast<double>(x);
        for (int i = 0; i < _poles; ++i) {
            _state[static_cast<size_t>(i)] += _a * (v - _state[static_cast<size_t>(i)]);
            v = _state[static_cast<size_t>(i)];
        }
        return static_cast<float>(v);
    }

    void processBlock(float* buf, size_t count) override {
        // Same math as process(), with the pole state held in a local
        // array across the loop -- the compiler can keep it in registers
        // instead of round-tripping _state through memory per sample.
        std::array<double, kMaxPoles> state = _state;
        const double a = _a;
        const int poles = _poles;
        for (size_t n = 0; n < count; ++n) {
            double v = static_cast<double>(buf[n]);
            for (int i = 0; i < poles; ++i) {
                state[static_cast<size_t>(i)] += a * (v - state[static_cast<size_t>(i)]);
                v = state[static_cast<size_t>(i)];
            }
            buf[n] = static_cast<float>(v);
        }
        _state = state;
    }

    void retune(double cutoffHz, int /*resonanceCode*/) override {
        const double clampedCutoff = std::min(cutoffHz, _sampleRateHz * 0.49);
        _a = 1.0 - std::exp(-2.0 * M_PI * clampedCutoff / _sampleRateHz);
    }

    void reset() override {
        _state.fill(0.0);
    }

private:
    int _poles;
    double _sampleRateHz;
    double _a = 0.0;
    std::array<double, kMaxPoles> _state;
};

// The exact difference equation from the reverse-engineered
// l7a1045_l6028_dsp_a.cpp (S2000/S3000/S3200 voice chip). See
// FilterModel.cpp's original comment for the k<=1.1 stability margin.
class ChamberlinSVF : public IFilterStage {
public:
    ChamberlinSVF(double cutoffHz, int resonanceCode, double sampleRateHz)
        : _sampleRateHz(sampleRateHz) {
        retune(cutoffHz, resonanceCode);
    }

    float process(float x) override {
        const double h = static_cast<double>(x) - _low - _damping * _band;
        _band += _k * h;
        _low += _k * _band;

        // Hard safety backstop -- see FilterModel.cpp's original comment.
        constexpr double kStateLimit = 100.0;
        _band = std::max(-kStateLimit, std::min(kStateLimit, _band));
        _low = std::max(-kStateLimit, std::min(kStateLimit, _low));

        return static_cast<float>(_low);
    }

    void processBlock(float* buf, size_t count) override {
        double low = _low, band = _band;
        const double k = _k, damping = _damping;
        constexpr double kStateLimit = 100.0;
        for (size_t n = 0; n < count; ++n) {
            const double h = static_cast<double>(buf[n]) - low - damping * band;
            band += k * h;
            low += k * band;
            band = std::max(-kStateLimit, std::min(kStateLimit, band));
            low = std::max(-kStateLimit, std::min(kStateLimit, low));
            buf[n] = static_cast<float>(low);
        }
        _low = low;
        _band = band;
    }

    void retune(double cutoffHz, int resonanceCode) override {
        const double kRaw = 2.0 * std::sin(M_PI * std::min(cutoffHz, _sampleRateHz * 0.49) / _sampleRateHz);
        _k = std::min(kRaw, 1.1);
        const int clampedRes = std::max(0, std::min(15, resonanceCode));
        _damping = 1.0 - static_cast<double>(clampedRes) / 16.0;
    }

    void reset() override {
        _low = 0.0;
        _band = 0.0;
    }

private:
    double _sampleRateHz;
    double _k = 0.0;
    double _damping = 1.0;
    double _low = 0.0;
    double _band = 0.0;
};

// Zero-delay-feedback state-variable filter (Zavalishin). See
// FilterModel.cpp's original comment for the passband-gain compensation
// derivation -- resonanceCompensation01 is a machine-profile constant,
// fixed at construction; only cutoff/resonance retune.
class TptSvf : public IFilterStage {
public:
    TptSvf(double cutoffHz, int resonanceCode, double sampleRateHz, double resonanceCompensation01)
        : _sampleRateHz(sampleRateHz), _resonanceCompensation01(resonanceCompensation01) {
        retune(cutoffHz, resonanceCode);
    }

    float process(float x) override {
        const double v0 = static_cast<double>(x) * _inputScale;
        const double v3 = v0 - _ic2eq;
        const double v1 = _a1 * _ic1eq + _a2 * v3;
        const double v2 = _ic2eq + _a2 * _ic1eq + _a3 * v3;
        _ic1eq = 2.0 * v1 - _ic1eq;
        _ic2eq = 2.0 * v2 - _ic2eq;
        return static_cast<float>(filterStagesSoftKneeLimit(v2 * _outputMakeup));
    }

    void processBlock(float* buf, size_t count) override {
        double ic1eq = _ic1eq, ic2eq = _ic2eq;
        const double a1 = _a1, a2 = _a2, a3 = _a3;
        const double inputScale = _inputScale, outputMakeup = _outputMakeup;
        for (size_t n = 0; n < count; ++n) {
            const double v0 = static_cast<double>(buf[n]) * inputScale;
            const double v3 = v0 - ic2eq;
            const double v1 = a1 * ic1eq + a2 * v3;
            const double v2 = ic2eq + a2 * ic1eq + a3 * v3;
            ic1eq = 2.0 * v1 - ic1eq;
            ic2eq = 2.0 * v2 - ic2eq;
            buf[n] = static_cast<float>(filterStagesSoftKneeLimit(v2 * outputMakeup));
        }
        _ic1eq = ic1eq;
        _ic2eq = ic2eq;
    }

    void retune(double cutoffHz, int resonanceCode) override {
        const double clampedCutoff = std::min(cutoffHz, _sampleRateHz * 0.49);
        const double g = std::tan(M_PI * clampedCutoff / _sampleRateHz);

        const int clampedRes = std::max(0, std::min(15, resonanceCode));
        const double damping = 1.0 - static_cast<double>(clampedRes) / 16.0;
        _k = 2.0 * damping;

        _a1 = 1.0 / (1.0 + g * (g + _k));
        _a2 = g * _a1;
        _a3 = g * _a2;

        const double peakGain = std::max(1.0, 1.0 / std::max(_k, 1e-6));
        _inputScale = 1.0 / (1.0 + _resonanceCompensation01 * (peakGain - 1.0));
        _outputMakeup = std::sqrt(1.0 / _inputScale);
    }

    void reset() override {
        _ic1eq = 0.0;
        _ic2eq = 0.0;
    }

private:
    double _sampleRateHz;
    double _resonanceCompensation01;
    double _k = 2.0;
    double _a1 = 0.0, _a2 = 0.0, _a3 = 0.0;
    double _inputScale = 1.0;
    double _outputMakeup = 1.0;
    double _ic1eq = 0.0, _ic2eq = 0.0;
};

// Stilson & Smith 4-pole transistor ladder stand-in (SSM2044/SSM2045).
// See FilterModel.cpp's original comment.
class SsmLadder : public IFilterStage {
public:
    SsmLadder(double cutoffHz, int resonanceCode, double sampleRateHz, double resonanceCompensation01)
        : _sampleRateHz(sampleRateHz), _resonanceCompensation01(resonanceCompensation01) {
        retune(cutoffHz, resonanceCode);
    }

    float process(float x) override {
        const double feedback = std::tanh(_resonanceAmount * _stage[3]);
        double v = static_cast<double>(x) * _inputScale - feedback;
        for (int i = 0; i < 4; ++i) {
            _stage[i] += _g * (v - _stage[i]);
            v = _stage[i];
        }

        constexpr double kStateLimit = 100.0;
        for (double& s : _stage) {
            s = std::max(-kStateLimit, std::min(kStateLimit, s));
        }

        return static_cast<float>(filterStagesSoftKneeLimit(_stage[3] * _outputMakeup));
    }

    void processBlock(float* buf, size_t count) override {
        double stage[4] = {_stage[0], _stage[1], _stage[2], _stage[3]};
        const double g = _g, resonanceAmount = _resonanceAmount;
        const double inputScale = _inputScale, outputMakeup = _outputMakeup;
        constexpr double kStateLimit = 100.0;
        for (size_t n = 0; n < count; ++n) {
            const double feedback = std::tanh(resonanceAmount * stage[3]);
            double v = static_cast<double>(buf[n]) * inputScale - feedback;
            for (int i = 0; i < 4; ++i) {
                stage[i] += g * (v - stage[i]);
                v = stage[i];
            }
            for (double& s : stage) {
                s = std::max(-kStateLimit, std::min(kStateLimit, s));
            }
            buf[n] = static_cast<float>(filterStagesSoftKneeLimit(stage[3] * outputMakeup));
        }
        for (int i = 0; i < 4; ++i) {
            _stage[i] = stage[i];
        }
    }

    void retune(double cutoffHz, int resonanceCode) override {
        const double clampedCutoff = std::min(cutoffHz, _sampleRateHz * 0.49);
        _g = 1.0 - std::exp(-2.0 * M_PI * clampedCutoff / _sampleRateHz);

        const int clampedRes = std::max(0, std::min(15, resonanceCode));
        _resonanceAmount = 4.0 * (static_cast<double>(clampedRes) / 15.0);

        const double peakGain = std::max(1.0, 1.0 + _resonanceAmount * 0.9);
        _inputScale = 1.0 / (1.0 + _resonanceCompensation01 * (peakGain - 1.0));
        _outputMakeup = std::sqrt(1.0 / _inputScale);
    }

    void reset() override {
        for (double& s : _stage) s = 0.0;
    }

private:
    double _sampleRateHz;
    double _resonanceCompensation01;
    double _g = 0.0;
    double _resonanceAmount = 0.0;
    double _inputScale = 1.0;
    double _outputMakeup = 1.0;
    double _stage[4] = {0.0, 0.0, 0.0, 0.0};
};

// Instantiates one stage of `topology` -- see FilterModel.cpp's original
// comment for the per-argument applicability notes.
inline std::unique_ptr<IFilterStage> makeFilterStage(AkzFilterTopology topology, double cutoffHz, int resonanceCode, double sampleRateHz, int poles, double resonanceCompensation01) {
    switch (topology) {
        case AkzFilterTopology_ChamberlinSvf:
            return std::make_unique<ChamberlinSVF>(cutoffHz, resonanceCode, sampleRateHz);
        case AkzFilterTopology_TptSvf:
        case AkzFilterTopology_CemStateVariable:
            return std::make_unique<TptSvf>(cutoffHz, resonanceCode, sampleRateHz, resonanceCompensation01);
        case AkzFilterTopology_SsmLadder:
            return std::make_unique<SsmLadder>(cutoffHz, resonanceCode, sampleRateHz, resonanceCompensation01);
        case AkzFilterTopology_OnePoleCascade:
        default:
            return std::make_unique<OnePoleLowpassCascade>(poles, cutoffHz, sampleRateHz);
    }
}

} // namespace akz

#endif // AKAIZER_FILTER_STAGES_H
