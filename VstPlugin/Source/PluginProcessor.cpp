// PluginProcessor.cpp
//
// See PluginProcessor.h.

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

namespace {

constexpr const char* kMachineParamId = "machine";
constexpr const char* kBitDepthParamId = "bitDepth";
constexpr const char* kBandwidthParamId = "bandwidth";
constexpr const char* kCutoffParamId = "cutoff";
constexpr const char* kResonanceParamId = "resonance";
constexpr const char* kMixParamId = "mix";
constexpr const char* kOutputParamId = "outputDb";

} // namespace

juce::StringArray PatinaFXAudioProcessor::machineChoices() {
    juce::StringArray choices;
    for (size_t i = 0; i < akz_machine_count(); ++i) {
        choices.add(akz_machine_profile(static_cast<AkzMachine>(i))->name);
    }
    return choices;
}

juce::String PatinaFXAudioProcessor::stableIdForMachine(AkzMachine machine) {
    return akz_machine_profile(machine)->stableId;
}

AkzMachine PatinaFXAudioProcessor::machineForStableId(const juce::String& stableId) {
    for (size_t i = 0; i < akz_machine_count(); ++i) {
        const AkzMachine machine = static_cast<AkzMachine>(i);
        if (stableId == akz_machine_profile(machine)->stableId) {
            return machine;
        }
    }
    return AkzMachine_S950; // fallback -- an unrecognised id (older/newer roster) lands on a sensible default rather than machine 0
}

PatinaFXAudioProcessor::PatinaFXAudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", _createParameterLayout()) {
    // Cache the atomic pointers once -- see .h. getRawParameterValue()
    // is a string-keyed map lookup; doing it per processBlock() is the
    // classic JUCE audio-thread waste.
    _machineParam = apvts.getRawParameterValue(kMachineParamId);
    _bitDepthParam = apvts.getRawParameterValue(kBitDepthParamId);
    _bandwidthParam = apvts.getRawParameterValue(kBandwidthParamId);
    _cutoffParam = apvts.getRawParameterValue(kCutoffParamId);
    _resonanceParam = apvts.getRawParameterValue(kResonanceParamId);
    _mixParam = apvts.getRawParameterValue(kMixParamId);
    _outputParam = apvts.getRawParameterValue(kOutputParamId);
}

PatinaFXAudioProcessor::~PatinaFXAudioProcessor() {
    _destroyChannels();
}

juce::AudioProcessorValueTreeState::ParameterLayout PatinaFXAudioProcessor::_createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Defaults resolve to the S950 (index into machineChoices() below
    // matches AkzMachine_S950's enum value, since choices are built in
    // enum order) -- a reasonable flagship default, not load-bearing:
    // the real record of "which machine" for a saved project is the
    // stableId attribute getStateInformation adds, not this index.
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        kMachineParamId, "Machine", machineChoices(), static_cast<int>(AkzMachine_S950)));

    // 0 = machine's own native bit depth -- see AkaizerCore.h's
    // AkzRealtimeChannelParams.bitDepth doc. The core caps any positive
    // override AT native (crusher-only), so values above the selected
    // machine's depth are legal but resolve to native.
    layout.add(std::make_unique<juce::AudioParameterInt>(
        kBitDepthParamId, "Bit Depth", 0, 24, 0,
        juce::AudioParameterIntAttributes().withStringFromValueFunction(
            [](int value, int) {
                return value <= 0 ? juce::String("Native")
                                  : juce::String(value) + " bit";
            })));

    // Absolute Hz, deliberately NOT scoped to any one machine's own
    // [min,max] -- akz_realtime_channel_process resolves/clamps this
    // into whichever machine is currently selected (RateModel.h's
    // resolveSampleRateHz), so one fixed control range works for every
    // machine without the parameter itself needing to change shape when
    // the machine does. The 7000 Hz floor sits just below the lowest
    // machine minimum (Fairlight's 7040) so there's no dead travel
    // under every machine's clamp; the 48000 default resolves to each
    // machine's own maxSampleRateHz, matching the app's
    // akz_stretch_params_default convention. On a dual-fixed-rate
    // machine (S1000/S2000/S3000/S3200) the resolve step snaps to the
    // nearer of its two real rates -- see MachineControls.h's
    // machineHasBandwidthControl for why the knob shows there.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        kBandwidthParamId, "Bandwidth",
        juce::NormalisableRange<float>(7000.0f, 48000.0f, 1.0f, 0.35f), // skewed so the musically dense low end isn't cramped
        48000.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float value, int) { return juce::String(static_cast<int>(std::lround(value))) + " Hz"; })));

    // 0..1 knobs: two decimals, matching the app's own "%.2f" knob
    // convention (MachineControls.swift) rather than JUCE's default
    // three-plus-decimal display.
    const auto twoDecimals = juce::AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float value, int) { return juce::String(value, 2); });

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        kCutoffParamId, "Cutoff", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f,
        twoDecimals));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        kResonanceParamId, "Resonance", juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
        twoDecimals));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        kMixParamId, "Mix", juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float value, int) { return juce::String(static_cast<int>(std::lround(value * 100.0f))) + " %"; })));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        kOutputParamId, "Output", juce::NormalisableRange<float>(-24.0f, 24.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float value, int) { return juce::String(value, 1) + " dB"; })));

    return layout;
}

void PatinaFXAudioProcessor::_destroyChannels() {
    for (auto* channel : _channels) {
        akz_realtime_channel_destroy(channel);
    }
    _channels.clear();
}

void PatinaFXAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    _destroyChannels();
    const int channelCount = juce::jmax(getTotalNumInputChannels(), getTotalNumOutputChannels(), 1);
    _channels.reserve(static_cast<size_t>(channelCount));
    for (int i = 0; i < channelCount; ++i) {
        _channels.push_back(akz_realtime_channel_create(sampleRate, static_cast<size_t>(samplesPerBlock)));
    }
    _dryBuffer.setSize(channelCount, samplesPerBlock, false, false, true);

    // ~20ms ramps, matching the core's own control smoothing feel.
    // Snap to the current values so playback starts un-ramped.
    _mixSmoothed.reset(sampleRate, 0.02);
    _outputGainSmoothed.reset(sampleRate, 0.02);
    _mixSmoothed.setCurrentAndTargetValue(_mixParam->load());
    _outputGainSmoothed.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(_outputParam->load()));
    _hasSentParams = false; // channels were just rebuilt -- first processBlock must push params regardless
}

void PatinaFXAudioProcessor::releaseResources() {
    _destroyChannels();
}

bool PatinaFXAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo()) {
        return false;
    }
    return mainOut == layouts.getMainInputChannelSet();
}

AkzRealtimeChannelParams PatinaFXAudioProcessor::_currentParams() const {
    AkzRealtimeChannelParams params{};
    const int rawMachineIndex = static_cast<int>(_machineParam->load());
    const int clampedIndex = juce::jlimit(0, static_cast<int>(akz_machine_count()) - 1, rawMachineIndex);
    params.machine = static_cast<AkzMachine>(clampedIndex);
    params.bitDepth = static_cast<int>(_bitDepthParam->load());
    params.sampleRateHz = _bandwidthParam->load();
    params.filterCutoff01 = _cutoffParam->load();
    params.filterResonance01 = _resonanceParam->load();
    return params;
}

void PatinaFXAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const AkzRealtimeChannelParams params = _currentParams();

    // Push params only when they actually changed -- setParams takes a
    // mutex + copies a POD per channel, which is pointless work on the
    // (overwhelmingly common) block where every knob is idle.
    const bool paramsChanged = !_hasSentParams
        || params.machine != _lastSentParams.machine
        || params.bitDepth != _lastSentParams.bitDepth
        || params.sampleRateHz != _lastSentParams.sampleRateHz
        || params.filterCutoff01 != _lastSentParams.filterCutoff01
        || params.filterResonance01 != _lastSentParams.filterResonance01;

    _mixSmoothed.setTargetValue(_mixParam->load());
    _outputGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(_outputParam->load()));

    // needsDry tracks the ramp's ENDPOINT, not its current value: a mix
    // ramping 1 -> 0 still needs the dry copy for the whole descent.
    const bool needsDry = _mixParam->load() < 0.999f || _mixSmoothed.getCurrentValue() < 0.999f;

    if (needsDry) {
        _dryBuffer.setSize(numChannels, numSamples, false, false, true);
        for (int ch = 0; ch < numChannels; ++ch) {
            _dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);
        }
    }

    for (int ch = 0; ch < numChannels && ch < static_cast<int>(_channels.size()); ++ch) {
        if (paramsChanged) {
            akz_realtime_channel_set_params(_channels[static_cast<size_t>(ch)], &params);
        }
        akz_realtime_channel_process(_channels[static_cast<size_t>(ch)], buffer.getWritePointer(ch), static_cast<size_t>(numSamples));
    }
    _lastSentParams = params;
    _hasSentParams = true;

    for (int ch = 0; ch < numChannels; ++ch) {
        float* wet = buffer.getWritePointer(ch);
        if (needsDry) {
            const float* dry = _dryBuffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i) {
                const float mix = _mixSmoothed.getNextValue();
                const float outputGain = _outputGainSmoothed.getNextValue();
                wet[i] = (wet[i] * mix + dry[i] * (1.0f - mix)) * outputGain;
            }
        } else {
            for (int i = 0; i < numSamples; ++i) {
                wet[i] *= _outputGainSmoothed.getNextValue();
            }
            _mixSmoothed.skip(numSamples); // keep the ramp in step with the output gain's
        }
    }
}

juce::AudioProcessorEditor* PatinaFXAudioProcessor::createEditor() {
    return new PatinaFXAudioProcessorEditor(*this);
}

void PatinaFXAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    // The machine's stableId, ADDITIONALLY to the choice parameter's own
    // raw index the APVTS XML already stores -- see PresetStore.swift's
    // rule (a preset's machine survives AkzMachine being reordered
    // because it's keyed by stableId, never by enum value) and
    // machineForStableId's use in setStateInformation below.
    const int machineIndex = static_cast<int>(apvts.getRawParameterValue(kMachineParamId)->load());
    const int clampedIndex = juce::jlimit(0, static_cast<int>(akz_machine_count()) - 1, machineIndex);
    xml->setAttribute("machineStableId", stableIdForMachine(static_cast<AkzMachine>(clampedIndex)));

    copyXmlToBinary(*xml, destData);
}

void PatinaFXAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType())) {
        return;
    }

    apvts.replaceState(juce::ValueTree::fromXml(*xml));

    if (xml->hasAttribute("machineStableId")) {
        const AkzMachine resolved = machineForStableId(xml->getStringAttribute("machineStableId"));
        if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(kMachineParamId))) {
            *choiceParam = static_cast<int>(resolved);
        }
    }
}

// This creates the plugin's audio processor. See:
// https://docs.juce.com/master/tutorial_audio_processor.html
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new PatinaFXAudioProcessor();
}
