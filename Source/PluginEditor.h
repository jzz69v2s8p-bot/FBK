#pragma once

#include <array>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

//==============================================================================
// Zeigt Spektrum, gesetzte Filter und Alarm-LED eines Kanals.
class ChannelView : public juce::Component
{
public:
    ChannelView (fg::ChannelEngine& engine, int channelIndex);

    void tick();                              // vom Timer des Editors aufgerufen
    void paint (juce::Graphics&) override;

private:
    float xOf (double freq) const;            // 0..1 auf logarithmischer Achse

    fg::ChannelEngine& eng;
    int index;
    std::array<float, fg::kDisplayBins> smooth {};
    uint32_t lastDetections = 0;
    juce::uint32 flashUntil = 0;
};

//==============================================================================
class FeedbackGuardEditor : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit FeedbackGuardEditor (FeedbackGuardProcessor&);
    ~FeedbackGuardEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAtt> attachment;
    };

    void timerCallback() override;
    void addKnob (Knob& k, const juce::String& paramId, const juce::String& text);
    void logEvent (int channel, const fg::Event& e);

    FeedbackGuardProcessor& proc;
    juce::LookAndFeel_V4 laf;

    int numChannels = 2;
    int numShown = 2;
    std::vector<std::unique_ptr<ChannelView>> views;

    juce::ToggleButton activeBtn, monitorBtn, duckBtn;
    juce::TextButton resetBtn;
    std::unique_ptr<ButtonAtt> activeAtt, monitorAtt, duckAtt;

    std::array<Knob, 7> knobs;
    juce::ComboBox speedBox;
    juce::Label speedLabel;
    std::unique_ptr<ComboAtt> speedAtt;

    juce::Label statusLabel;
    juce::TextEditor log;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackGuardEditor)
};
