// PluginEditor.cpp
//
// See PluginEditor.h.

#include "PluginEditor.h"
#include "MachineControls.h"

namespace {

juce::Slider::SliderStyle kKnobStyle = juce::Slider::RotaryHorizontalVerticalDrag;

void configureKnob(juce::Slider& slider, juce::Label& label, const juce::String& text, juce::Component& parent) {
    slider.setSliderStyle(kKnobStyle);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
    parent.addAndMakeVisible(slider);

    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.attachToComponent(&slider, false);
    parent.addAndMakeVisible(label);
}

} // namespace

PatinaFXAudioProcessorEditor::PatinaFXAudioProcessorEditor(PatinaFXAudioProcessor& proc)
    : juce::AudioProcessorEditor(&proc), _processor(proc) {
    _machineBox.addItemList(PatinaFXAudioProcessor::machineChoices(), 1);
    addAndMakeVisible(_machineBox);
    _machineLabel.setText("Machine", juce::dontSendNotification);
    _machineLabel.setJustificationType(juce::Justification::centred);
    _machineLabel.attachToComponent(&_machineBox, false);
    addAndMakeVisible(_machineLabel);

    _readoutLabel.setJustificationType(juce::Justification::centred);
    _readoutLabel.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
    addAndMakeVisible(_readoutLabel);

    configureKnob(_bitDepthSlider, _bitDepthLabel, "Bit Depth", *this);
    configureKnob(_bandwidthSlider, _bandwidthLabel, "Bandwidth", *this);
    configureKnob(_cutoffSlider, _cutoffLabel, "Cutoff", *this);
    configureKnob(_resonanceSlider, _resonanceLabel, "Resonance", *this);
    configureKnob(_mixSlider, _mixLabel, "Mix", *this);
    configureKnob(_outputSlider, _outputLabel, "Output", *this);

    auto& apvts = _processor.apvts;
    _machineAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(apvts, "machine", _machineBox);
    _bitDepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "bitDepth", _bitDepthSlider);
    _bandwidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "bandwidth", _bandwidthSlider);
    _cutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "cutoff", _cutoffSlider);
    _resonanceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "resonance", _resonanceSlider);
    _mixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "mix", _mixSlider);
    _outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, "outputDb", _outputSlider);

    setSize(560, 260);
    startTimerHz(10); // control-rate poll for the machine combo + LCD readout, not audio-rate -- see timerCallback()
    _updateVisibilityAndReadout();
}

PatinaFXAudioProcessorEditor::~PatinaFXAudioProcessorEditor() {
    stopTimer();
}

void PatinaFXAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void PatinaFXAudioProcessorEditor::resized() {
    auto area = getLocalBounds().reduced(16);

    auto topRow = area.removeFromTop(48);
    _machineBox.setBounds(topRow.removeFromLeft(220).reduced(0, 12));
    topRow.removeFromLeft(12);
    _readoutLabel.setBounds(topRow);

    area.removeFromTop(28); // room for the knob labels attached above each slider

    const int knobCount = 6;
    const int knobWidth = area.getWidth() / knobCount;
    auto knobRow = area.removeFromTop(140);

    juce::Slider* knobs[knobCount] = {
        &_bitDepthSlider, &_bandwidthSlider, &_cutoffSlider, &_resonanceSlider, &_mixSlider, &_outputSlider
    };
    for (auto* knob : knobs) {
        knob->setBounds(knobRow.removeFromLeft(knobWidth).reduced(8));
    }
}

void PatinaFXAudioProcessorEditor::timerCallback() {
    _updateVisibilityAndReadout();
}

void PatinaFXAudioProcessorEditor::_updateVisibilityAndReadout() {
    const int machineIndex = juce::jlimit(0, static_cast<int>(akz_machine_count()) - 1, _machineBox.getSelectedItemIndex());
    const int bitDepth = static_cast<int>(_processor.apvts.getRawParameterValue("bitDepth")->load());
    if (machineIndex == _lastDisplayedMachineIndex && bitDepth == _lastDisplayedBitDepth) return;
    _lastDisplayedMachineIndex = machineIndex;
    _lastDisplayedBitDepth = bitDepth;

    const AkzMachine machine = static_cast<AkzMachine>(machineIndex);
    const AkzMachineProfile* profile = akz_machine_profile(machine);

    const bool showBandwidth = patinafx::machineHasBandwidthControl(machine);
    _bandwidthSlider.setVisible(showBandwidth);
    _bandwidthLabel.setVisible(showBandwidth);

    const bool showResonance = patinafx::machineHasResonanceControl(machine);
    _resonanceSlider.setVisible(showResonance);
    _resonanceLabel.setVisible(showResonance);

    // Effective bit depth, not just the knob's raw value: the core caps
    // any positive override AT the machine's native depth (a crusher,
    // not an upgrade), so "24 bit" selected on a 12-bit machine really
    // means 12 -- show the resolved figure so the cap is legible rather
    // than surprising.
    const int effectiveBits = bitDepth > 0 ? std::min(bitDepth, profile->bitDepth) : profile->bitDepth;
    const juce::String bitText = juce::String(effectiveBits) + "-bit"
        + (bitDepth > 0 && bitDepth < profile->bitDepth ? " crushed" : " native");

    // Dual-fixed-rate machines (S1000/S2000/S3000/S3200) snap the
    // bandwidth knob to their two real rates -- name them so the snap
    // is visible rather than felt as dead travel.
    const bool dualRate = profile->hasVariableSampleRate == 0
        && profile->minSampleRateHz < profile->maxSampleRateHz;
    const juce::String rateText = dualRate
        ? juce::String(profile->minSampleRateHz, 0) + "/" + juce::String(profile->maxSampleRateHz, 0) + " Hz"
        : juce::String(profile->maxSampleRateHz, 0) + " Hz";
    _bandwidthSlider.setTooltip(dualRate
        ? "This machine has two fixed rates -- the knob snaps to the nearer one."
        : "");

    _readoutLabel.setText(
        juce::String(profile->name) + "  ·  " + juce::String(profile->yearIntroduced)
            + "  ·  " + bitText + "  ·  " + rateText,
        juce::dontSendNotification);
}
