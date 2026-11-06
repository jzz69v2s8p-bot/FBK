#include "PluginProcessor.h"
#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;

juce::AudioProcessorValueTreeState::ParameterLayout FeedbackGuardProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "active", 1 }, "Aktiv", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "monitor", 1 }, "Nur beobachten", false));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sens", 1 }, "Empfindlichkeit",
                                                        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
                                                        AudioParameterFloatAttributes().withLabel ("%")));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "speed", 1 }, "Reaktion",
                                                         StringArray { "Vorsichtig", "Normal", "Schnell" }, 1));

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "maxnotches", 1 }, "Max. Filter", 1, fg::kMaxNotches, 8));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "depth", 1 }, "Max. Absenkung",
                                                        NormalisableRange<float> (6.0f, 30.0f, 1.0f), 18.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "hold", 1 }, "Haltezeit",
                                                        NormalisableRange<float> (10.0f, 600.0f, 1.0f, 0.4f), 60.0f,
                                                        AudioParameterFloatAttributes().withLabel ("s")));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "minf", 1 }, "Untere Grenze",
                                                        NormalisableRange<float> (80.0f, 2000.0f, 1.0f, 0.5f), 150.0f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz")));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "maxf", 1 }, "Obere Grenze",
                                                        NormalisableRange<float> (2000.0f, 16000.0f, 1.0f, 0.5f), 10000.0f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz")));

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "duck", 1 }, "Notbremse", true));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "duckdb", 1 }, "Notbremse Tiefe",
                                                        NormalisableRange<float> (0.0f, 12.0f, 0.5f), 6.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));

    return { p.begin(), p.end() };
}

FeedbackGuardProcessor::FeedbackGuardProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "FeedbackGuard", createLayout())
{
    pActive     = apvts.getRawParameterValue ("active");
    pMonitor    = apvts.getRawParameterValue ("monitor");
    pSens       = apvts.getRawParameterValue ("sens");
    pSpeed      = apvts.getRawParameterValue ("speed");
    pMaxNotches = apvts.getRawParameterValue ("maxnotches");
    pDepth      = apvts.getRawParameterValue ("depth");
    pHold       = apvts.getRawParameterValue ("hold");
    pMinF       = apvts.getRawParameterValue ("minf");
    pMaxF       = apvts.getRawParameterValue ("maxf");
    pDuck       = apvts.getRawParameterValue ("duck");
    pDuckDb     = apvts.getRawParameterValue ("duckdb");
}

bool FeedbackGuardProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out || in.isDisabled()) return false;
    return in.size() >= 1 && in.size() <= kMaxChannels;
}

void FeedbackGuardProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    for (auto& e : engines) e.prepare (sampleRate);
    setLatencySamples (0);
}

fg::Settings FeedbackGuardProcessor::readSettings() const
{
    fg::Settings s;
    s.enabled      = pActive->load() > 0.5f;
    s.monitorOnly  = pMonitor->load() > 0.5f;
    // Empfindlichkeit 0..100 % -> geforderte Überhöhung 22..6 dB (50 % = 14 dB)
    s.prominenceDb = 22.0f - 0.16f * pSens->load();
    s.speed        = (int) pSpeed->load();
    s.maxNotches   = (int) pMaxNotches->load();
    s.maxDepthDb   = pDepth->load();
    s.holdSeconds  = pHold->load();
    s.minFreq      = pMinF->load();
    s.maxFreq      = juce::jmax (pMaxF->load(), s.minFreq + 500.0f);
    s.duckEnabled  = pDuck->load() > 0.5f;
    s.duckDb       = pDuckDb->load();
    return s;
}

void FeedbackGuardProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels(), kMaxChannels);
    const int n = buffer.getNumSamples();

    if (resetRequested.exchange (false))
        for (auto& e : engines) e.reset();

    const auto s = readSettings();
    for (int ch = 0; ch < numCh; ++ch)
    {
        engines[(size_t) ch].setSettings (s);
        engines[(size_t) ch].process (buffer.getWritePointer (ch), n);
    }

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, n);
}

juce::AudioProcessorEditor* FeedbackGuardProcessor::createEditor()
{
    return new FeedbackGuardEditor (*this);
}

void FeedbackGuardProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void FeedbackGuardProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FeedbackGuardProcessor();
}
