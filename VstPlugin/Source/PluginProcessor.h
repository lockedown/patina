// PluginProcessor.h
//
// Patina FX: a VST3 insert effect wrapping Sources/Core's real-time
// signal path (Sources/Core/RealtimeChannel.h / the
// akz_realtime_channel_* C API in AkaizerCore.h). One AkzRealtimeChannel
// per audio channel, driven every processBlock() from the current
// APVTS parameter values -- see RealtimeChannel.h's own header comment
// for why this needs no cross-channel commit gate the way the app's
// live-audition player does (nothing here ever changes buffer length).
//
// Only ever includes Sources/Core/include/AkaizerCore.h, the one header
// that file's own comment says any consumer outside Sources/Core is
// allowed to include -- same boundary discipline the Swift app follows,
// applied to this, the app's second (and Windows-only) consumer of the
// same DSP core.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "include/AkaizerCore.h"

#include <vector>

class PatinaFXAudioProcessor : public juce::AudioProcessor {
public:
    PatinaFXAudioProcessor();
    ~PatinaFXAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Every machine's display name, in AkzMachine enum order -- the
    // "machine" AudioParameterChoice's index space. See
    // stableIdForMachine/machineForStableId for why the SAVED state
    // doesn't just trust this index across app versions.
    static juce::StringArray machineChoices();
    static juce::String stableIdForMachine(AkzMachine machine);
    static AkzMachine machineForStableId(const juce::String& stableId);

    juce::AudioProcessorValueTreeState apvts;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout _createParameterLayout();
    AkzRealtimeChannelParams _currentParams() const;
    void _destroyChannels();

    std::vector<AkzRealtimeChannel*> _channels;
    juce::AudioBuffer<float> _dryBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PatinaFXAudioProcessor)
};
