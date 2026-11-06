#pragma once

#include <array>
#include <juce_audio_processors/juce_audio_processors.h>

#include "FeedbackEngine.h"

class FeedbackGuardProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kMaxChannels = 8;

    FeedbackGuardProcessor();
    ~FeedbackGuardProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Von der Oberfläche aufgerufen
    void requestReset() { resetRequested.store (true); }

    juce::AudioProcessorValueTreeState apvts;
    std::array<fg::ChannelEngine, kMaxChannels> engines;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    fg::Settings readSettings() const;

    std::atomic<bool> resetRequested { false };
    double currentSampleRate = 48000.0;

    std::atomic<float>* pActive = nullptr;
    std::atomic<float>* pMonitor = nullptr;
    std::atomic<float>* pSens = nullptr;
    std::atomic<float>* pSpeed = nullptr;
    std::atomic<float>* pMaxNotches = nullptr;
    std::atomic<float>* pDepth = nullptr;
    std::atomic<float>* pHold = nullptr;
    std::atomic<float>* pMinF = nullptr;
    std::atomic<float>* pMaxF = nullptr;
    std::atomic<float>* pDuck = nullptr;
    std::atomic<float>* pDuckDb = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackGuardProcessor)
};
