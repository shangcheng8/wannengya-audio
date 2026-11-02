#pragma once

#include <JuceHeader.h>

#include "Config.h"
#include "Control.h"
#include "DriftBridge.h"
#include "RingBuffer.h"

#include <atomic>
#include <memory>
#include <string>

// wannengya Capture — replaces the host buffer with the tapped App and/or the
// selected virtual pair. Both sources arrive through shared memory from the
// background Agent; the plug-in only mixes and reports state.
class CaptureProcessor final : public juce::AudioProcessor,
                               private juce::Timer
{
public:
    CaptureProcessor();
    ~CaptureProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "wannengya Capture"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- selection -------------------------------------------------------
    void selectApp(int32_t pid, const juce::String& bundleID, const juce::String& name);
    void clearApp();
    void setUsePair(bool use);
    void selectPair(int pairIndex);

    bool appSelected() const noexcept;
    bool pairSelected() const noexcept;
    int selectedPair() const noexcept;
    juce::String appName() const;
    juce::String appBundleID() const;
    int32_t appPID() const noexcept;

    // ---- status ----------------------------------------------------------
    bool agentAlive() const noexcept;
    wng::State agentState() const noexcept;
    wng::Fault agentFault() const noexcept;
    wng::State tapState() const noexcept;
    wng::State channelState() const noexcept;
    wng::Fault tapFault() const noexcept;
    wng::Fault channelFault() const noexcept;
    int audioCapturePermissionRaw() const noexcept;
    int microphonePermissionRaw() const noexcept;

    float tapPeak() const noexcept;
    float channelPeak() const noexcept;
    float outputPeak() const noexcept;
    uint32_t tapRate() const noexcept;
    uint32_t channelRate() const noexcept;
    float tapGain() const noexcept;
    int driftPPM() const noexcept;
    uint64_t underruns() const noexcept;

    uint32_t hostRate() const noexcept { return static_cast<uint32_t>(std::llround(hostRate_)); }
    bool ringsOpen() const noexcept { return tapRing.isOpen() && channelRing.isOpen(); }

    void reconnect();

private:
    void timerCallback() override;
    bool openRings();
    void publish();
    void restore();
    void pump(bool wantApp, bool wantChannel, uint32_t frames);
    wng::ControlBlock* control() const noexcept;

    wng::ControlMapping mapping;
    wng::RingBuffer tapRing;
    wng::RingBuffer channelRing;

    std::vector<float> tapScratch;
    std::vector<float> channelScratch;
    std::vector<float> readScratch;

    wng::DriftBridge tapBridge;
    wng::DriftBridge channelBridge;

    std::atomic<float> outputPeakValue { 0.0f };
    std::atomic<uint64_t> processedFrames { 0 };

    double hostRate_ = 48000.0;

    juce::CriticalSection stateLock;
    juce::String savedBundleID;
    juce::String savedName;
    int32_t savedPID = -1;
    int savedPair = wng::kDefaultCapturePair;
    bool savedUsePair = true;
    bool restorePending = false;
    uint64_t lastRestoreMs = 0;
    int reconnectTicks = 0;
    int launchTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CaptureProcessor)
};
