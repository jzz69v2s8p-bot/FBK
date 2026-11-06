#include "PluginEditor.h"

namespace
{
const juce::Colour kBg      { 0xff101317 };
const juce::Colour kPanel   { 0xff171b21 };
const juce::Colour kAccent  { 0xff2fc4b2 };
const juce::Colour kAlarm   { 0xffff5a4f };
const juce::Colour kText    { 0xffdfe5ec };
const juce::Colour kDim     { 0xff7d8794 };

juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }
} // namespace

//==============================================================================
ChannelView::ChannelView (fg::ChannelEngine& e, int idx) : eng (e), index (idx)
{
    smooth.fill (-120.0f);
    lastDetections = eng.detections.load();
}

float ChannelView::xOf (double freq) const
{
    const double fHi = juce::jmin (16000.0, eng.getSampleRate() * 0.45);
    return (float) juce::jlimit (0.0, 1.0, std::log (freq / 50.0) / std::log (fHi / 50.0));
}

void ChannelView::tick()
{
    for (size_t d = 0; d < smooth.size(); ++d)
    {
        const float v = eng.uiSpectrum[d].load (std::memory_order_relaxed);
        smooth[d] = v > smooth[d] ? v : smooth[d] - 1.2f;      // schnell hoch, langsam runter
    }

    const auto det = eng.detections.load();
    if (det != lastDetections)
    {
        lastDetections = det;
        flashUntil = juce::Time::getMillisecondCounter() + 1500;
    }
    repaint();
}

void ChannelView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (b, 6.0f);

    const auto plot = b.reduced (10.0f, 0.0f).withTrimmedTop (26.0f).withTrimmedBottom (16.0f);

    // Gitter
    g.setFont (10.0f);
    for (double f : { 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const float x = plot.getX() + xOf (f) * plot.getWidth();
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        if (f == 100.0 || f == 1000.0 || f == 10000.0 || f == 500.0 || f == 5000.0)
        {
            g.setColour (kDim);
            g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + " kHz" : juce::String ((int) f) + " Hz",
                        (int) x - 30, (int) plot.getBottom() + 1, 60, 14, juce::Justification::centred);
        }
    }

    auto yOfDb = [&] (float db)
    {
        const float t = (juce::jlimit (-110.0f, -10.0f, db) + 110.0f) / 100.0f;
        return plot.getBottom() - t * plot.getHeight();
    };
    for (float db : { -90.0f, -70.0f, -50.0f, -30.0f })
    {
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawHorizontalLine ((int) yOfDb (db), plot.getX(), plot.getRight());
    }

    // Spektrum
    juce::Path fill, line;
    fill.startNewSubPath (plot.getX(), plot.getBottom());
    for (size_t d = 0; d < smooth.size(); ++d)
    {
        const float x = plot.getX() + ((float) d + 0.5f) / (float) smooth.size() * plot.getWidth();
        const float y = yOfDb (smooth[d]);
        fill.lineTo (x, y);
        if (d == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
    }
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.closeSubPath();
    g.setColour (kAccent.withAlpha (0.20f));
    g.fillPath (fill);
    g.setColour (kAccent);
    g.strokePath (line, juce::PathStrokeType (1.4f));

    // Gesetzte Filter
    g.setFont (11.0f);
    int shown = 0;
    for (int i = 0; i < fg::kMaxNotches; ++i)
    {
        const float f = eng.uiNotchFreq[(size_t) i].load (std::memory_order_relaxed);
        if (f <= 0.0f) continue;
        const float depth = eng.uiNotchDepth[(size_t) i].load (std::memory_order_relaxed);
        const float x = plot.getX() + xOf (f) * plot.getWidth();
        const float alpha = juce::jlimit (0.1f, 0.45f, -depth / 30.0f);
        g.setColour (kAlarm.withAlpha (alpha));
        g.fillRect (x - 3.0f, plot.getY(), 6.0f, plot.getHeight());
        g.setColour (kAlarm);
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());

        const juce::String txt = juce::String ((int) std::round (f)) + " Hz  " + juce::String (depth, 0) + " dB";
        const int ty = (int) plot.getY() + 2 + (shown++ % 4) * 12;
        const bool right = x < plot.getCentreX();
        g.drawText (txt, right ? (int) x + 5 : (int) x - 105, ty, 100, 12,
                    right ? juce::Justification::left : juce::Justification::right);
    }

    // Kopfzeile
    g.setColour (kText);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("Kanal " + juce::String (index + 1), 12, 5, 80, 18, juce::Justification::left);

    const bool alarm = juce::Time::getMillisecondCounter() < flashUntil;
    g.setColour (alarm ? kAlarm : juce::Colour (0xff2c4d3f));
    g.fillEllipse (82.0f, 9.0f, 11.0f, 11.0f);
    g.setColour (alarm ? kAlarm : kDim);
    g.setFont (11.0f);
    g.drawText (alarm ? "Feedback erkannt" : "ruhig", 98, 6, 120, 16, juce::Justification::left);

    const float lvl = eng.uiLevelDb.load (std::memory_order_relaxed);
    const float duck = eng.uiDuckDb.load (std::memory_order_relaxed);
    juce::String info = "Pegel " + (lvl <= -100.0f ? juce::String ("-inf") : juce::String (lvl, 1)) + " dBFS";
    if (duck < -0.5f) info += "   Notbremse " + juce::String (duck, 1) + " dB";
    g.setColour (duck < -0.5f ? kAlarm : kDim);
    g.drawText (info, (int) b.getRight() - 260, 6, 250, 16, juce::Justification::right);
}

//==============================================================================
FeedbackGuardEditor::FeedbackGuardEditor (FeedbackGuardProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    laf.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
    laf.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
    laf.setColour (juce::ToggleButton::tickColourId, kAccent);
    setLookAndFeel (&laf);

    numChannels = juce::jlimit (1, FeedbackGuardProcessor::kMaxChannels, p.getTotalNumInputChannels());
    numShown    = juce::jmin (numChannels, 4);

    for (int i = 0; i < numShown; ++i)
    {
        views.push_back (std::make_unique<ChannelView> (proc.engines[(size_t) i], i));
        addAndMakeVisible (*views.back());
    }

    auto setupToggle = [this] (juce::ToggleButton& b, const juce::String& text)
    {
        b.setButtonText (text);
        addAndMakeVisible (b);
    };
    setupToggle (activeBtn, "Aktiv");
    setupToggle (monitorBtn, "Nur beobachten");
    setupToggle (duckBtn, "Notbremse");
    activeAtt  = std::make_unique<ButtonAtt> (proc.apvts, "active", activeBtn);
    monitorAtt = std::make_unique<ButtonAtt> (proc.apvts, "monitor", monitorBtn);
    duckAtt    = std::make_unique<ButtonAtt> (proc.apvts, "duck", duckBtn);
    monitorBtn.setTooltip (utf8 ("Erkennt und zeigt Feedback an, greift aber nicht ein (zum Testen)."));

    resetBtn.setButtonText (utf8 ("Alle Filter zurücksetzen"));
    resetBtn.onClick = [this]
    {
        proc.requestReset();
        log.moveCaretToEnd();
        log.insertTextAtCaret (juce::Time::getCurrentTime().toString (false, true) + utf8 ("  Alle Filter zur\xc3\xbc" "ckgesetzt\n"));
    };
    addAndMakeVisible (resetBtn);

    addKnob (knobs[0], "sens",       "Empfindlichkeit");
    addKnob (knobs[1], "depth",      "Max. Absenkung");
    addKnob (knobs[2], "hold",       "Haltezeit");
    addKnob (knobs[3], "maxnotches", "Max. Filter");
    addKnob (knobs[4], "minf",       "Untere Grenze");
    addKnob (knobs[5], "maxf",       "Obere Grenze");
    addKnob (knobs[6], "duckdb",     "Notbremse");

    speedBox.addItemList ({ "Vorsichtig", "Normal", "Schnell" }, 1);
    addAndMakeVisible (speedBox);
    speedAtt = std::make_unique<ComboAtt> (proc.apvts, "speed", speedBox);
    speedLabel.setText ("Reaktion", juce::dontSendNotification);
    speedLabel.setJustificationType (juce::Justification::centred);
    speedLabel.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (speedLabel);

    statusLabel.setColour (juce::Label::textColourId, kDim);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    log.setMultiLine (true);
    log.setReadOnly (true);
    log.setScrollbarsShown (true);
    log.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    log.setColour (juce::TextEditor::backgroundColourId, kPanel);
    log.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (log);

    const int h = 56 + numShown * 150 + 8 + 2 * 112 + 8 + 96 + 18;
    setSize (820, h);
    startTimerHz (30);
}

FeedbackGuardEditor::~FeedbackGuardEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void FeedbackGuardEditor::addKnob (Knob& k, const juce::String& paramId, const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
    addAndMakeVisible (k.slider);
    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (k.label);
    k.attachment = std::make_unique<SliderAtt> (proc.apvts, paramId, k.slider);
}

void FeedbackGuardEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("Feedback Guard", 16, 10, 300, 28, juce::Justification::left);
    g.setColour (kDim);
    g.setFont (11.0f);
    g.drawText (utf8 ("erkennt Rückkopplung und setzt schmale Filter, Sprache bleibt unberührt"), 16, 34, 420, 14, juce::Justification::left);
}

void FeedbackGuardEditor::resized()
{
    auto r = getLocalBounds().reduced (10, 0);

    auto top = r.removeFromTop (52).withTrimmedTop (8);
    resetBtn.setBounds (top.removeFromRight (190).reduced (0, 6));
    top.removeFromRight (8);
    duckBtn.setBounds (top.removeFromRight (100));
    monitorBtn.setBounds (top.removeFromRight (130));
    activeBtn.setBounds (top.removeFromRight (80));

    for (auto& v : views)
    {
        v->setBounds (r.removeFromTop (146));
        r.removeFromTop (4);
    }
    r.removeFromTop (4);

    // 2 Reihen mit je 4 Zellen (7 Regler + Auswahlfeld)
    const int cellW = r.getWidth() / 4;
    for (int row = 0; row < 2; ++row)
    {
        auto rowArea = r.removeFromTop (112);
        for (int col = 0; col < 4; ++col)
        {
            auto cell = rowArea.removeFromLeft (cellW).reduced (6, 0);
            const int idx = row * 4 + col;
            if (idx < 7)
            {
                knobs[(size_t) idx].label.setBounds (cell.removeFromTop (16));
                knobs[(size_t) idx].slider.setBounds (cell);
            }
            else
            {
                speedLabel.setBounds (cell.removeFromTop (16));
                speedBox.setBounds (cell.removeFromTop (60).reduced (10, 18));
                statusLabel.setBounds (cell);
            }
        }
    }

    r.removeFromTop (8);
    log.setBounds (r.removeFromTop (96));
}

void FeedbackGuardEditor::logEvent (int channel, const fg::Event& e)
{
    static const char* rules[] = { "schnelles Aufschaukeln", "wachsender Ton", "stehender Ton" };
    juce::String line = juce::Time::getCurrentTime().toString (false, true) + "  Kanal " + juce::String (channel + 1)
                      + "  " + juce::String ((int) std::round (e.freq)) + " Hz  (" + rules[juce::jlimit (0, 2, (int) e.rule)] + ", +"
                      + juce::String (e.prominenceDb, 0) + " dB)";
    if (e.monitorOnly)    line += "  -> nur beobachtet";
    else if (e.deepened)  line += "  -> Filter vertieft auf " + juce::String (e.depthDb, 0) + " dB";
    else                  line += "  -> Filter gesetzt " + juce::String (e.depthDb, 0) + " dB";

    log.moveCaretToEnd();
    log.insertTextAtCaret (line + "\n");
}

void FeedbackGuardEditor::timerCallback()
{
    for (auto& v : views) v->tick();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        fg::Event e;
        while (proc.engines[(size_t) ch].popEvent (e))
            logEvent (ch, e);
    }

    if (numChannels > numShown && statusLabel.getText().isEmpty())
        statusLabel.setText (juce::String (numChannels - numShown) + utf8 (" weitere Kan\xc3\xa4le werden mitverarbeitet"),
                             juce::dontSendNotification);
}
