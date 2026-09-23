// PluginEditor.h
//
// Deliberately plain: rotary sliders + a combo box, no attempt at
// visually cloning the SwiftUI app's rack-panel look (RotaryKnobView.swift,
// LCDReadoutView.swift) for v1 -- see the project plan's note that a
// visual clone isn't needed yet. What IS ported is the app's data-driven
// visibility rule (MachineControls.h/.cpp): Bandwidth and Resonance
// sliders show or hide per selected machine's capability flags, same as
// MachineControls.swift drives ContentView's knob row.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class PatinaFXAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PatinaFXAudioProcessorEditor(PatinaFXAudioProcessor&);
    ~PatinaFXAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void _updateVisibilityAndReadout();

    PatinaFXAudioProcessor& _processor;

    juce::ComboBox _machineBox;
    juce::Label _machineLabel;
    juce::Label _readoutLabel; // "S950 · 1990 · 12-bit · 30000 Hz"-style LCD-ish line

    juce::Slider _bitDepthSlider;
    juce::Label _bitDepthLabel;
    juce::Slider _bandwidthSlider;
    juce::Label _bandwidthLabel;
    juce::Slider _cutoffSlider;
    juce::Label _cutoffLabel;
    juce::Slider _resonanceSlider;
    juce::Label _resonanceLabel;
    juce::Slider _mixSlider;
    juce::Label _mixLabel;
    juce::Slider _outputSlider;
    juce::Label _outputLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> _machineAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> _bitDepthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> _bandwidthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> _cutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> _resonanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> _mixAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> _outputAttachment;

    int _lastDisplayedMachineIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PatinaFXAudioProcessorEditor)
};
