// DATLAB: DATTRACK's Sample Lab as an audio plugin.
// Offline (non-realtime) processing of a sample into a new sample, previewed through the track and dragged into Ableton.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "dsp/LabEngine.h"

struct LabSample
{
    lab::Chans ch; double sr = 44100; juce::String name; juce::File file;
    bool empty() const { return ch.empty() || ch[0].empty(); }
    double seconds() const { return empty() ? 0 : (double) ch[0].size() / sr; }
};

class DatLabProcessor : public juce::AudioProcessor, public juce::ChangeBroadcaster, private juce::Thread, private juce::Timer
{
public:
    DatLabProcessor();
    ~DatLabProcessor() override;

    // ---- juce::AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "DATLAB"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- lab (message thread) ----
    juce::String procId = "paul";
    std::map<juce::String, lab::Params> values;     // per-process parameter values
    lab::Shape shape;
    lab::Params& paramsFor (const juce::String& id);
    const lab::ProcDef& proc() const;

    LabSample source, aux, result;
    std::vector<LabSample> history;                  // CHAIN pushes the previous source here, UNDO brings it back
    juce::String status = "Load or drop a sample (or CAPTURE the track), pick a process, then PROCESS";

    bool loadFile (const juce::File&, bool asAux, juce::String& error);
    bool readFile (const juce::File&, LabSample& into, juce::String& error);
    void setSource (LabSample s, bool pushHistory);
    void chainResult();
    void undoChain();
    void startProcessing();
    void cancelProcessing();
    bool isBusy() const { return busy.load(); }
    float getProgress() const { return progress.load(); }
    void randomize();
    void resetParams();
    juce::File renderFolder() const;

    // preview through the plugin output
    enum class Play { none, source, result };
    void play (Play what);
    void stopPlay();
    Play nowPlaying() const { return playing.load(); }
    float playPosition() const;                     // 0..1 of the playing buffer
    std::atomic<bool> loopPreview { false }, thru { true };

    // capture the track's input into a new source
    void startCapture();
    void stopCapture();
    bool isCapturing() const { return capturing.load(); }
    double captureSeconds() const { return capSr > 0 ? (double) capPos.load() / capSr : 0; }

    // tempo for synced processes
    std::atomic<double> hostBpm { 0 };
    bool bpmFromHost = true; double manualBpm = 120;
    double effectiveBpm() const { double h = hostBpm.load(); return bpmFromHost && h > 0 ? h : manualBpm; }

    juce::AudioFormatManager formats;

    // tells the host the plugin state changed, so Ableton offers to save the set
    void markDirty();

private:
    void run() override;   // background render
    void timerCallback() override;
    static juce::File writeWav (const LabSample&, const juce::File&);

    // Preview buffer: swapped by the message thread under a lock the audio thread only try-locks,
    // so the audio thread never blocks and never frees a (possibly huge) buffer.
    struct PlayBuf { lab::Chans ch; double sr = 44100; };
    std::unique_ptr<PlayBuf> playBuf;
    juce::SpinLock playLock;
    std::atomic<Play> playing { Play::none };
    std::atomic<int> playRequest { 0 };             // bumped by play(); the audio thread then rewinds
    int playSeen = 0; double playPos = 0;           // audio thread only
    std::atomic<float> playFrac { 0 };
    std::atomic<double> hostSr { 44100 };

    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::atomic<bool> busy { false }; std::atomic<float> progress { 0 };
    lab::Job pendingJob; juce::String pendingName, pendingTag;

    juce::AudioBuffer<float> capBuf; std::atomic<int> capPos { 0 }; std::atomic<bool> capturing { false }; double capSr = 0;
    bool capPending = false;                        // a take is recording or waiting to become the source
    std::atomic<bool> capWriting { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DatLabProcessor)
};
