#include "PluginProcessor.h"

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout MidiCaptureProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    StringArray channels { "Omni (all channels)" };
    for (int i = 1; i <= 16; ++i) channels.add ("Channel " + String (i));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "channel", 1 }, "Input Channel", channels, 0));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "bars", 1 }, "Loop Length (Bars)", 1, 16, 4));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "quant", 1 }, "Quantize On Export",
        StringArray { "Off", "1/4", "1/8", "1/16", "1/32" }, 3));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "record", 1 }, "Record", false));

    return { std::make_move_iterator (p.begin()), std::make_move_iterator (p.end()) };
}

//==============================================================================
MidiCaptureProcessor::MidiCaptureProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pChannel = apvts.getRawParameterValue ("channel");
    pBars    = apvts.getRawParameterValue ("bars");
    pQuant   = apvts.getRawParameterValue ("quant");
    pRecord  = apvts.getRawParameterValue ("record");

    startTimerHz (30);
}

MidiCaptureProcessor::~MidiCaptureProcessor()
{
    stopTimer();
}

void MidiCaptureProcessor::prepareToPlay (double, int)
{
    wasRecording = false;
    freeRunPpq = 0.0;
    held.fill (false);
}

bool MidiCaptureProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

//==============================================================================
void MidiCaptureProcessor::pushEvent (const RawEvent& e)
{
    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)
    {
        fifoData[(size_t) s1] = e;
        fifo.finishedWrite (1);
    }
}

void MidiCaptureProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const int numSamples = buffer.getNumSamples();

    // --- transport / timing ----------------------------------------------------
    bool playing = false;
    double ppqStart = freeRunPpq, bpm = 120.0;

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            bpm = pos->getBpm().orFallback (120.0);
            if (pos->getIsPlaying())
                if (auto pp = pos->getPpqPosition())
                {
                    ppqStart = *pp;
                    playing = true;
                }
        }

    if (bpm <= 0.0) bpm = 120.0;

    const bool rec = pRecord->load() > 0.5f;

    // transport stopped: start recording from bar 1 of the loop
    if (rec && ! wasRecording && ! playing)
    {
        freeRunPpq = 0.0;
        ppqStart = 0.0;
    }

    const double samplesPerPpq = getSampleRate() * 60.0 / bpm;
    const double loopLen = juce::jlimit (1, 16, (int) pBars->load()) * 4.0;

    auto wrap = [loopLen] (double ppq)
    {
        double w = std::fmod (ppq, loopLen);
        if (w < 0.0) w += loopLen;
        return w;
    };

    // recording switched off: close every note that is still held
    if (! rec && wasRecording)
    {
        for (int i = 0; i < (int) held.size(); ++i)
            if (held[(size_t) i])
            {
                pushEvent ({ wrap (ppqStart), i % 128, 0, i / 128 + 1, false });
                held[(size_t) i] = false;
            }
    }
    wasRecording = rec;

    // --- capture incoming notes --------------------------------------------------
    if (rec)
    {
        const int chFilter = (int) pChannel->load();   // 0 = omni

        for (const auto metadata : midi)
        {
            const auto msg = metadata.getMessage();
            if (! (msg.isNoteOn() || msg.isNoteOff())) continue;
            if (chFilter != 0 && msg.getChannel() != chFilter) continue;

            const double ppq = wrap (ppqStart + metadata.samplePosition / samplesPerPpq);
            const int ch = msg.getChannel();
            const int note = msg.getNoteNumber();
            const size_t idx = (size_t) ((ch - 1) * 128 + note);

            if (msg.isNoteOn())
            {
                pushEvent ({ ppq, note, (int) msg.getVelocity(), ch, true });
                held[idx] = true;
            }
            else
            {
                pushEvent ({ ppq, note, 0, ch, false });
                held[idx] = false;
            }
        }
    }

    freeRunPpq = ppqStart + numSamples / samplesPerPpq;
    midi.clear();
}

//==============================================================================
void MidiCaptureProcessor::drainFifo()
{
    const double loopLen = getLoopLengthPpq();

    auto closeNote = [this, loopLen] (int key, double endPpq, int note, int ch)
    {
        auto it = openNotes.find (key);
        if (it == openNotes.end()) return;

        const double start = it->second.start;
        double len = endPpq >= start ? endPpq - start : endPpq + loopLen - start;
        if (len < 0.02) len = 0.02;

        if (notes.size() < 20000)
            notes.push_back ({ start, len, note, it->second.velocity, ch });

        openNotes.erase (it);
    };

    while (fifo.getNumReady() > 0)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (1, s1, n1, s2, n2);
        const RawEvent e = fifoData[(size_t) (n1 > 0 ? s1 : s2)];
        fifo.finishedRead (n1 + n2);

        const int key = e.channel * 128 + e.note;

        if (e.isOn)
        {
            closeNote (key, e.ppq, e.note, e.channel);   // retrigger of a held note
            openNotes[key] = { e.ppq, e.velocity };
        }
        else
        {
            closeNote (key, e.ppq, e.note, e.channel);
        }
    }
}

void MidiCaptureProcessor::clearPattern()
{
    drainFifo();
    notes.clear();
    openNotes.clear();
}

//==============================================================================
juce::File MidiCaptureProcessor::getTempExportFile() const
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("MIDI Capture")
               .getChildFile ("Captured Pattern.mid");
}

bool MidiCaptureProcessor::exportMidi (const juce::File& target) const
{
    const double loopLen = getLoopLengthPpq();
    const int tpq = 960;

    static const double grids[] = { 0.0, 1.0, 0.5, 0.25, 0.125 };
    const double grid = grids[juce::jlimit (0, 4, (int) pQuant->load())];

    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4));

    for (const auto& n : notes)
    {
        if (n.start >= loopLen) continue;

        double s = n.start;
        double len = n.length;

        if (grid > 0.0)
        {
            s = std::round (s / grid) * grid;
            len = std::max (grid, std::round (len / grid) * grid);
        }

        if (s >= loopLen) s -= loopLen;
        len = std::min (len, loopLen - s);
        if (len <= 0.01) continue;

        auto on = juce::MidiMessage::noteOn (n.channel, n.note, (juce::uint8) n.velocity);
        on.setTimeStamp (s * tpq);
        auto off = juce::MidiMessage::noteOff (n.channel, n.note);
        off.setTimeStamp ((s + len) * tpq);

        seq.addEvent (on);
        seq.addEvent (off);
    }

    auto endMarker = juce::MidiMessage::endOfTrack();
    endMarker.setTimeStamp (loopLen * tpq);
    seq.addEvent (endMarker);
    seq.updateMatchedPairs();

    juce::MidiFile file;
    file.setTicksPerQuarterNote (tpq);
    file.addTrack (seq);

    target.getParentDirectory().createDirectory();
    target.deleteFile();

    juce::FileOutputStream out (target);
    if (! out.openedOk()) return false;
    return file.writeTo (out);
}

//==============================================================================
// Editor
namespace
{
    class PianoRoll : public juce::Component
    {
    public:
        explicit PianoRoll (MidiCaptureProcessor& p) : proc (p) {}

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.fillAll (juce::Colour (0xff181818));

            const double loopLen = proc.getLoopLengthPpq();
            const auto& notes = proc.getNotes();

            for (int i = 0; i <= (int) loopLen; ++i)
            {
                const float x = b.getX() + b.getWidth() * (float) (i / loopLen);
                g.setColour (i % 4 == 0 ? juce::Colour (0xff555555) : juce::Colour (0xff2a2a2a));
                g.drawVerticalLine ((int) x, b.getY(), b.getBottom());
            }

            int lo = 127, hi = 0;
            for (const auto& n : notes)
            {
                lo = std::min (lo, n.note);
                hi = std::max (hi, n.note);
            }
            if (notes.empty()) { lo = 48; hi = 72; }
            if (hi - lo < 11)  hi = lo + 11;

            const float rowH = b.getHeight() / (float) (hi - lo + 1);

            g.setColour (juce::Colour (0xffff9b3d));
            for (const auto& n : notes)
            {
                if (n.start >= loopLen) continue;
                const float x = b.getX() + b.getWidth() * (float) (n.start / loopLen);
                const float w = std::max (2.0f, b.getWidth() * (float) (n.length / loopLen));
                const float y = b.getBottom() - (float) (n.note - lo + 1) * rowH;
                g.fillRect (x, y, std::min (w, b.getRight() - x), std::max (2.0f, rowH - 1.0f));
            }

            if (notes.empty())
            {
                g.setColour (juce::Colours::grey);
                g.setFont (15.0f);
                g.drawText ("Enable Record and play MIDI on the selected channel",
                            getLocalBounds(), juce::Justification::centred);
            }
        }

    private:
        MidiCaptureProcessor& proc;
    };

    class DragPad : public juce::Component
    {
    public:
        explicit DragPad (MidiCaptureProcessor& p) : proc (p) {}

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (1.0f);
            const bool empty = proc.getNotes().empty();
            g.setColour (empty ? juce::Colour (0xff2a2a2a) : juce::Colour (0xff2e6b3a));
            g.fillRoundedRectangle (b, 8.0f);
            g.setColour (juce::Colours::white.withAlpha (empty ? 0.4f : 0.95f));
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText (empty ? "Nothing recorded yet" : "Drag this MIDI clip into your DAW",
                        getLocalBounds(), juce::Justification::centred);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging || e.getDistanceFromDragStart() < 8) return;
            if (proc.getNotes().empty()) return;

            const auto file = proc.getTempExportFile();
            if (! proc.exportMidi (file)) return;

            dragging = true;
            juce::DragAndDropContainer::performExternalDragDropOfFiles (
                juce::StringArray (file.getFullPathName()), false, this,
                [safe = juce::Component::SafePointer<DragPad> (this)] { if (safe != nullptr) safe->dragging = false; });
        }

        void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    private:
        MidiCaptureProcessor& proc;
        bool dragging = false;
    };
}

class MidiCaptureEditor : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit MidiCaptureEditor (MidiCaptureProcessor& p)
        : AudioProcessorEditor (&p), proc (p), roll (p), pad (p)
    {
        for (auto* l : { &channelLabel, &barsLabel, &quantLabel })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (*l);
        }
        channelLabel.setText ("Input", juce::dontSendNotification);
        barsLabel.setText ("Bars", juce::dontSendNotification);
        quantLabel.setText ("Quantize", juce::dontSendNotification);

        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("channel")))
            channelBox.addItemList (cp->choices, 1);
        if (auto* qp = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("quant")))
            quantBox.addItemList (qp->choices, 1);

        barsSlider.setSliderStyle (juce::Slider::IncDecButtons);
        barsSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 24);

        recButton.setButtonText ("Record");
        clearButton.setButtonText ("Clear");
        saveButton.setButtonText ("Save .mid...");

        channelAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "channel", channelBox);
        quantAtt   = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "quant", quantBox);
        barsAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "bars", barsSlider);
        recAtt     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "record", recButton);

        clearButton.onClick = [this] { proc.clearPattern(); repaint(); };
        saveButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Save MIDI clip",
                juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Captured Pattern.mid"),
                "*.mid");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [p = &proc] (const juce::FileChooser& fc)
                                  {
                                      const auto f = fc.getResult();
                                      if (f != juce::File())
                                          p->exportMidi (f.withFileExtension ("mid"));
                                  });
        };

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &channelBox, &quantBox, &barsSlider, &recButton, &clearButton,
                 &saveButton, &roll, &pad })
            addAndMakeVisible (*c);

        setSize (600, 420);
        startTimerHz (15);
    }

    ~MidiCaptureEditor() override { stopTimer(); }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff101010)); }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10);

        auto row1 = b.removeFromTop (28);
        const int colW = row1.getWidth() / 3;
        auto c1 = row1.removeFromLeft (colW).reduced (4, 0);
        auto c2 = row1.removeFromLeft (colW).reduced (4, 0);
        auto c3 = row1.reduced (4, 0);
        channelLabel.setBounds (c1.removeFromLeft (50));  channelBox.setBounds (c1);
        barsLabel.setBounds (c2.removeFromLeft (40));     barsSlider.setBounds (c2);
        quantLabel.setBounds (c3.removeFromLeft (60));    quantBox.setBounds (c3);

        b.removeFromTop (8);
        auto row2 = b.removeFromTop (32);
        recButton.setBounds (row2.removeFromLeft (110));
        row2.removeFromLeft (8);
        clearButton.setBounds (row2.removeFromLeft (90));
        row2.removeFromLeft (8);
        saveButton.setBounds (row2.removeFromLeft (130));

        b.removeFromTop (8);
        pad.setBounds (b.removeFromBottom (64));
        b.removeFromBottom (8);
        roll.setBounds (b);
    }

private:
    void timerCallback() override
    {
        roll.repaint();
        pad.repaint();
    }

    MidiCaptureProcessor& proc;

    juce::Label channelLabel, barsLabel, quantLabel;
    juce::ComboBox channelBox, quantBox;
    juce::Slider barsSlider;
    juce::ToggleButton recButton;
    juce::TextButton clearButton, saveButton;
    PianoRoll roll;
    DragPad pad;
    std::unique_ptr<juce::FileChooser> chooser;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> channelAtt, quantAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> barsAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> recAtt;
};

juce::AudioProcessorEditor* MidiCaptureProcessor::createEditor()
{
    return new MidiCaptureEditor (*this);
}

//==============================================================================
void MidiCaptureProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    auto old = state.getChildWithName ("NOTES");
    if (old.isValid()) state.removeChild (old, nullptr);

    juce::ValueTree noteTree ("NOTES");
    for (const auto& n : notes)
    {
        juce::ValueTree v ("N");
        v.setProperty ("s", n.start, nullptr);
        v.setProperty ("l", n.length, nullptr);
        v.setProperty ("n", n.note, nullptr);
        v.setProperty ("v", n.velocity, nullptr);
        v.setProperty ("c", n.channel, nullptr);
        noteTree.appendChild (v, nullptr);
    }
    state.appendChild (noteTree, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MidiCaptureProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        if (! tree.isValid() || ! tree.hasType (apvts.state.getType())) return;

        apvts.replaceState (tree);

        notes.clear();
        openNotes.clear();

        auto noteTree = tree.getChildWithName ("NOTES");
        for (int i = 0; i < noteTree.getNumChildren(); ++i)
        {
            auto v = noteTree.getChild (i);
            notes.push_back ({ (double) v["s"], (double) v["l"], (int) v["n"], (int) v["v"], (int) v["c"] });
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MidiCaptureProcessor();
}
