#include "PluginProcessor.h"

juce::AudioProcessorValueTreeState::ParameterLayout MidiCaptureProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    StringArray channels { "Omni (all channels)" };
    for (int i = 1; i <= 16; ++i) channels.add ("Channel " + String (i));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "channel", 1 }, "Input Channel", channels, 0));

    ClipCapture::addParameters (p, 3);   // default quantize on export: 1/16

    return { std::make_move_iterator (p.begin()), std::make_move_iterator (p.end()) };
}

MidiCaptureProcessor::MidiCaptureProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pChannel = apvts.getRawParameterValue ("channel");
}

void MidiCaptureProcessor::prepareToPlay (double, int) {}

bool MidiCaptureProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void MidiCaptureProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // records the incoming notes; keeps its own clock running if the transport is stopped
    capture.processBlock (getPlayHead(), midi, buffer.getNumSamples(), getSampleRate(),
                          (int) pChannel->load(), true);
    midi.clear();
}

//==============================================================================
class MidiCaptureEditor : public juce::AudioProcessorEditor
{
public:
    explicit MidiCaptureEditor (MidiCaptureProcessor& p)
        : AudioProcessorEditor (&p), generic (p), clipUI (p.capture)
    {
        addAndMakeVisible (clipUI);
        addAndMakeVisible (generic);
        setSize (460, 700);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        clipUI.setBounds (b.removeFromTop (250));
        generic.setBounds (b);
    }

private:
    juce::GenericAudioProcessorEditor generic;
    ClipCaptureUI clipUI;
};

juce::AudioProcessorEditor* MidiCaptureProcessor::createEditor()
{
    return new MidiCaptureEditor (*this);
}

//==============================================================================
void MidiCaptureProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    capture.saveToTree (state);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MidiCaptureProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (tree);
            capture.loadFromTree (tree);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MidiCaptureProcessor();
}
