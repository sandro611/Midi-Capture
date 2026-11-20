#pragma once
#include <array>
#include <map>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>

struct RecNote
{
    double start;     // position inside the loop, in quarter notes
    double length;    // in quarter notes
    int note;
    int velocity;
    int channel;
};

class MidiCaptureProcessor : public juce::AudioProcessor,
                             private juce::Timer
{
public:
    MidiCaptureProcessor();
    ~MidiCaptureProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MIDI Capture"; }
    bool acceptsMidi() const override { return true; }
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

    // --- message-thread API used by the editor ---------------------------------
    const std::vector<RecNote>& getNotes() const { return notes; }
    double getLoopLengthPpq() const { return juce::jlimit (1, 16, (int) pBars->load()) * 4.0; }
    void clearPattern();
    bool exportMidi (const juce::File& target) const;
    juce::File getTempExportFile() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    struct RawEvent { double ppq; int note; int velocity; int channel; bool isOn; };

    void timerCallback() override { drainFifo(); }
    void drainFifo();
    void pushEvent (const RawEvent& e);

    std::atomic<float> *pChannel, *pBars, *pQuant, *pRecord;

    // audio thread -> message thread hand-off
    static constexpr int fifoSize = 4096;
    juce::AbstractFifo fifo { fifoSize };
    std::array<RawEvent, (size_t) fifoSize> fifoData {};

    // audio-thread state
    bool wasRecording = false;
    double freeRunPpq = 0.0;
    std::array<bool, 2048> held {};

    // message-thread state (recorded pattern)
    struct OpenNote { double start; int velocity; };
    std::vector<RecNote> notes;
    std::map<int, OpenNote> openNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiCaptureProcessor)
};
