#include "CaptureProcessor.h"

#include "SystemCompat.h"

#include "AgentLauncher.h"
#include "CaptureEditor.h"
#include "ProcessList.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kControlTimerMs = 200;
constexpr int kLaunchIntervalTicks = 10;   // 2 s at 200 ms

wng::CaptureMode modeFor(bool app, bool channel)
{
    if (app && channel) return wng::CaptureMode::Both;
    if (app) return wng::CaptureMode::App;
    if (channel) return wng::CaptureMode::Channel;
    return wng::CaptureMode::Off;
}

} // namespace

CaptureProcessor::CaptureProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    setLatencySamples(0);
    openRings();
    startTimer(kControlTimerMs);

    // Announce ourselves immediately: the Agent uses this heartbeat to know a
    // plug-in is alive and to release a tap whose owner disappeared.
    if (auto* c = control())
        c->captureHeartbeat.store(wng::nowNs(), std::memory_order_release);
}

CaptureProcessor::~CaptureProcessor()
{
    stopTimer();

    // The heartbeat also expires on its own; this is just the fast path.
    // Leaving a tap running (and the App muted) after the plug-in unloads would
    // be worse than a silent tail, so release the App selection explicitly.
    if (auto* c = control())
    {
        if (c->appPID.load(std::memory_order_relaxed) > 0)
        {
            c->appPID.store(-1, std::memory_order_relaxed);
            c->appObjectID.store(0, std::memory_order_relaxed);
            c->mode.store(static_cast<uint32_t>(wng::CaptureMode::Off), std::memory_order_relaxed);
            c->captureSeq.fetch_add(1, std::memory_order_release);
        }
    }
}

//==================================================================================================
#pragma mark Setup
//==================================================================================================

wng::ControlBlock* CaptureProcessor::control() const noexcept
{
    return mapping.block();
}

bool CaptureProcessor::openRings()
{
    mapping.open();

    tapRing.open(wng::tapRingName()) || tapRing.create(wng::tapRingName(), wng::kDefaultRate);
    channelRing.open(wng::channelRingName()) || channelRing.create(wng::channelRingName(), wng::kDefaultRate);

    return tapRing.isOpen() && channelRing.isOpen();
}

void CaptureProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    hostRate_ = sampleRate > 0 ? sampleRate : 48000.0;

    const auto frames = static_cast<size_t>(juce::jmax(samplesPerBlock, 2048));
    tapScratch.assign(frames * 2, 0.0f);
    channelScratch.assign(frames * 2, 0.0f);
    readScratch.assign(frames * 2, 0.0f);

    const auto rate = static_cast<uint32_t>(std::llround(hostRate_));
    tapBridge.reset(rate);
    channelBridge.reset(rate);
    tapBridge.setTarget(wng::kTargetFrames);
    channelBridge.setTarget(wng::kTargetFrames);

    if (! ringsOpen())
        openRings();

    if (tapRing.isOpen())
        tapRing.setHostSampleRate(rate);

    if (channelRing.isOpen())
        channelRing.setHostSampleRate(rate);

    wng::launchAgentIfNeeded();
    publish();
}

void CaptureProcessor::releaseResources() {}

bool CaptureProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();

    return (output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo())
           && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

//==================================================================================================
#pragma mark Audio
//==================================================================================================

void CaptureProcessor::pump(bool wantApp, bool wantChannel, uint32_t frames)
{
    if (tapRing.isOpen())
    {
        const uint32_t available = tapRing.available();

        if (wantApp && available > 0)
        {
            const uint32_t take = std::min(available, frames + 64);

            if (static_cast<size_t>(take) * 2 <= readScratch.size())
            {
                tapRing.read(readScratch.data(), take);
                tapBridge.push(readScratch.data(), take);
            }
        }

        if (wantApp && tapRing.sampleRate() != 0)
            tapBridge.setInputRate(tapRing.sampleRate());
    }

    if (channelRing.isOpen())
    {
        const uint32_t available = channelRing.available();

        if (wantChannel && available > 0)
        {
            const uint32_t take = std::min(available, frames + 64);

            if (static_cast<size_t>(take) * 2 <= readScratch.size())
            {
                channelRing.read(readScratch.data(), take);
                channelBridge.push(readScratch.data(), take);
            }
        }

        if (wantChannel && channelRing.sampleRate() != 0)
            channelBridge.setInputRate(channelRing.sampleRate());
    }
}

void CaptureProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int numSamples = buffer.getNumSamples();

    if (numSamples <= 0)
        return;

    // This insert replaces the track input entirely: the room microphone on the
    // same bus must not leak into the recording.
    buffer.clear();

    auto* c = control();

    if (c == nullptr || static_cast<size_t>(numSamples) * 2 > tapScratch.size())
    {
        outputPeakValue.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const auto rawMode = c->mode.load(std::memory_order_relaxed);
    const bool wantApp = rawMode == static_cast<uint32_t>(wng::CaptureMode::App)
                         || rawMode == static_cast<uint32_t>(wng::CaptureMode::Both);
    const bool wantChannel = rawMode == static_cast<uint32_t>(wng::CaptureMode::Channel)
                             || rawMode == static_cast<uint32_t>(wng::CaptureMode::Both);

    pump(wantApp, wantChannel, static_cast<uint32_t>(numSamples));

    const uint32_t tapRate = tapRing.sampleRate();
    const bool tapRateOk = tapRate == 0
                           || std::abs(static_cast<double>(tapRate) - hostRate_) <= 0.5;

    if (wantApp && tapRing.isOpen() && tapRateOk)
        tapBridge.pull(tapScratch.data(), static_cast<uint32_t>(numSamples));
    else
        std::fill(tapScratch.begin(), tapScratch.begin() + numSamples * 2, 0.0f);

    if (wantChannel && channelRing.isOpen())
        channelBridge.pull(channelScratch.data(), static_cast<uint32_t>(numSamples));
    else
        std::fill(channelScratch.begin(), channelScratch.begin() + numSamples * 2, 0.0f);

    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = tapScratch[static_cast<size_t>(i) * 2] + channelScratch[static_cast<size_t>(i) * 2];
        const float r = tapScratch[static_cast<size_t>(i) * 2 + 1] + channelScratch[static_cast<size_t>(i) * 2 + 1];

        if (right != nullptr)
        {
            // No clamp: float32 all the way through, so a hot App keeps its
            // transients instead of being flattened here.
            left[i] = l;
            right[i] = r;
            peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
        }
        else
        {
            const float mono = (l + r) * 0.5f;
            left[i] = mono;
            peak = std::max(peak, std::abs(mono));
        }
    }

    outputPeakValue.store(peak, std::memory_order_relaxed);
    processedFrames.fetch_add(static_cast<uint64_t>(numSamples), std::memory_order_relaxed);
}

void CaptureProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Keep the capture flowing even when the host natively bypasses the insert.
    processBlock(buffer, midi);
}

juce::AudioProcessorEditor* CaptureProcessor::createEditor()
{
    return new CaptureEditor(*this);
}

//==================================================================================================
#pragma mark State
//==================================================================================================

void CaptureProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Deliberately avoid juce::ValueTree/var here. A ValueTree created inside
    // this plug-in used to abort in its destructor while the host was asking
    // for state during instantiation (the VST3 wrapper calls getState() right
    // after createInstance). Keep the state as plain little-endian primitives:
    // no ref-counted var, no cross-module juce::String destruction.
    constexpr juce::int32 stateMagic = 0x574E4743;   // 'WNGC'
    constexpr juce::int16 stateVersion = 2;

    juce::MemoryOutputStream stream (destData, false);
    stream.writeInt (stateMagic);
    stream.writeShort (stateVersion);

    {
        const juce::ScopedLock lock (stateLock);
        stream.writeString (savedBundleID);
        stream.writeString (savedName);
        stream.writeInt (savedPID);
        stream.writeInt (savedPair);
        stream.writeBool (savedUsePair);
    }
}

void CaptureProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    juce::MemoryInputStream stream (data, static_cast<size_t> (sizeInBytes), false);

    // Version 1 states were ValueTree blobs written by older builds. There is
    // no safe way to parse those through var without risking the same abort,
    // so ignore them and start with the default selection instead.
    if (stream.readInt() != 0x574E4743)
        return;

    const auto version = stream.readShort();

    if (version < 2 || version > 2)
        return;

    const auto bundleID = stream.readString();
    const auto name = stream.readString();
    const auto pid = stream.readInt();
    const auto pair = stream.readInt();
    const auto usePair = stream.readBool();

    if (stream.getNumBytesRemaining() < 0)
        return;

    const juce::ScopedLock lock (stateLock);
    savedBundleID = bundleID;
    savedName = name;
    savedPID = pid;
    savedPair = juce::jlimit (0, wng::kDevicePairs - 1, pair);
    savedUsePair = usePair;
    restorePending = true;
    lastRestoreMs = 0;
}

//==================================================================================================
#pragma mark Selection
//==================================================================================================

void CaptureProcessor::selectApp(int32_t pid, const juce::String& bundleID, const juce::String& name)
{
    // A project saved on 14.2+ may still name an App when it is opened on macOS
    // 12 or 13. Remember the choice, but never ask the Agent for a tap there.
    const bool supported = wng::systemSupportsAppCapture();

    if (! supported)
        pid = -1;

    {
        const juce::ScopedLock lock(stateLock);
        savedPID = pid;
        savedBundleID = supported ? bundleID : juce::String();
        savedName = supported ? name : juce::String();
        restorePending = false;
    }

    publish();
}

void CaptureProcessor::clearApp()
{
    selectApp(-1, {}, {});
}

void CaptureProcessor::setUsePair(bool use)
{
    {
        const juce::ScopedLock lock(stateLock);
        savedUsePair = use;
        restorePending = false;
    }

    publish();
}

void CaptureProcessor::selectPair(int pairIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        savedPair = juce::jlimit(0, wng::kDevicePairs - 1, pairIndex);
        savedUsePair = true;
        restorePending = false;
    }

    publish();
}

void CaptureProcessor::publish()
{
    auto* c = control();

    if (c == nullptr)
        return;

    juce::String bundleID;
    juce::String name;
    int32_t pid = -1;
    int pair = wng::kDefaultCapturePair;
    bool usePair = true;

    {
        const juce::ScopedLock lock(stateLock);
        bundleID = savedBundleID;
        name = savedName;
        pid = savedPID;
        pair = juce::jlimit(0, wng::kDevicePairs - 1, savedPair);
        usePair = savedUsePair;
    }

    const auto rate = static_cast<uint32_t>(std::llround(hostRate_));

    c->appPID.store(pid, std::memory_order_relaxed);
    c->appObjectID.store(pid > 0 ? processObjectForPID(pid) : 0u, std::memory_order_relaxed);
    c->capturePair.store(pair, std::memory_order_relaxed);
    c->hostRate.store(rate, std::memory_order_relaxed);
    wng::copyText(c->appBundleID, sizeof(c->appBundleID), bundleID.toStdString());
    wng::copyText(c->appName, sizeof(c->appName), name.toStdString());
    c->mode.store(static_cast<uint32_t>(modeFor(pid > 0, usePair)), std::memory_order_relaxed);
    c->captureSeq.fetch_add(1, std::memory_order_release);

    if (tapRing.isOpen())
        tapRing.setHostSampleRate(rate);

    if (channelRing.isOpen())
        channelRing.setHostSampleRate(rate);
}

void CaptureProcessor::restore()
{
    juce::String bundleID;
    juce::String name;
    int32_t pid = -1;

    {
        const juce::ScopedLock lock(stateLock);

        if (! restorePending)
            return;

        const auto now = juce::Time::getMillisecondCounter();

        if (lastRestoreMs != 0 && now - lastRestoreMs < 3000)
            return;

        lastRestoreMs = now;
        bundleID = savedBundleID;
        name = savedName;
        pid = savedPID;
    }

    bool resolved = false;

    if (pid > 0 || bundleID.isNotEmpty())
    {
        const auto processes = listAudioProcesses(false);
        const AudioProcessInfo* match = nullptr;

        if (bundleID.isNotEmpty())
        {
            for (const auto& process : processes)
            {
                if (juce::String::fromUTF8(process.bundleID.c_str()) != bundleID)
                    continue;

                if (match == nullptr || (process.audioRunning && ! match->audioRunning))
                    match = &process;
            }
        }

        if (match == nullptr && pid > 0)
        {
            for (const auto& process : processes)
            {
                if (process.pid == pid)
                {
                    match = &process;
                    break;
                }
            }
        }

        if (match != nullptr)
        {
            selectApp(match->pid,
                      juce::String::fromUTF8(match->bundleID.c_str()),
                      name.isNotEmpty() ? name : juce::String::fromUTF8(match->name.c_str()));
            resolved = true;
        }
    }

    {
        const juce::ScopedLock lock(stateLock);
        restorePending = ! resolved && pid > 0;   // keep trying if the App is not up yet
    }

    publish();
}

//==================================================================================================
#pragma mark Timer
//==================================================================================================

void CaptureProcessor::timerCallback()
{
    if (! ringsOpen())
    {
        if (++reconnectTicks >= 10)
        {
            reconnectTicks = 0;
            openRings();
        }
    }
    else
    {
        reconnectTicks = 0;
    }

    if (auto* c = control())
        c->captureHeartbeat.store(wng::nowNs(), std::memory_order_release);

    restore();

    const auto rate = static_cast<uint32_t>(std::llround(hostRate_));

    if (tapRing.isOpen())
        tapRing.setHostSampleRate(rate);

    if (channelRing.isOpen())
        channelRing.setHostSampleRate(rate);

    if (! agentAlive())
    {
        if (++launchTicks >= kLaunchIntervalTicks)
        {
            launchTicks = 0;
            wng::launchAgentIfNeeded();
        }
    }
    else
    {
        launchTicks = 0;
    }

    // Process object IDs change when the App starts a new audio session even
    // while the pid stays the same.
    if (auto* c = control())
    {
        const int32_t pid = c->appPID.load(std::memory_order_relaxed);

        if (pid > 0)
        {
            const uint32_t object = processObjectForPID(pid);

            if (object != 0 && object != c->appObjectID.load(std::memory_order_relaxed))
            {
                c->appObjectID.store(object, std::memory_order_relaxed);
                c->captureSeq.fetch_add(1, std::memory_order_release);
            }
        }
    }
}

void CaptureProcessor::reconnect()
{
    openRings();
    publish();
}

//==================================================================================================
#pragma mark Status
//==================================================================================================

bool CaptureProcessor::appSelected() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedPID > 0;
}

bool CaptureProcessor::pairSelected() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedUsePair;
}

int CaptureProcessor::selectedPair() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedPair;
}

juce::String CaptureProcessor::appName() const
{
    const juce::ScopedLock lock(stateLock);
    return savedName;
}

juce::String CaptureProcessor::appBundleID() const
{
    const juce::ScopedLock lock(stateLock);
    return savedBundleID;
}

int32_t CaptureProcessor::appPID() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedPID;
}

bool CaptureProcessor::agentAlive() const noexcept
{
    const auto* c = control();

    if (c == nullptr)
        return false;

    const uint64_t heartbeat = c->heartbeat.load(std::memory_order_acquire);

    if (heartbeat == 0)
        return false;

    return wng::nowNs() - heartbeat < 2'000'000'000ull;
}

wng::State CaptureProcessor::agentState() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<wng::State>(c->state.load(std::memory_order_relaxed))
                        : wng::State::Stopped;
}

wng::Fault CaptureProcessor::agentFault() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<wng::Fault>(c->fault.load(std::memory_order_relaxed))
                        : wng::Fault::None;
}

wng::State CaptureProcessor::tapState() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<wng::State>(c->tapState.load(std::memory_order_relaxed))
                        : wng::State::Stopped;
}

wng::State CaptureProcessor::channelState() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<wng::State>(c->channelState.load(std::memory_order_relaxed))
                        : wng::State::Stopped;
}

wng::Fault CaptureProcessor::tapFault() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<wng::Fault>(c->tapFault.load(std::memory_order_relaxed))
                        : wng::Fault::None;
}

wng::Fault CaptureProcessor::channelFault() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<wng::Fault>(c->channelFault.load(std::memory_order_relaxed))
                        : wng::Fault::None;
}

int CaptureProcessor::audioCapturePermissionRaw() const noexcept
{
    const auto* c = control();

    if (c == nullptr)
        return -1;

    const uint32_t raw = c->permissionRaw.load(std::memory_order_relaxed);
    return raw == 0xFFFFFFFFu ? -1 : static_cast<int>(raw);
}

int CaptureProcessor::microphonePermissionRaw() const noexcept
{
    const auto* c = control();

    if (c == nullptr)
        return -1;

    const uint32_t raw = c->microphoneRaw.load(std::memory_order_relaxed);
    return raw == 0xFFFFFFFFu ? -1 : static_cast<int>(raw);
}

float CaptureProcessor::tapPeak() const noexcept
{
    const auto* c = control();
    return c != nullptr ? c->tapPeak.load(std::memory_order_relaxed) : 0.0f;
}

float CaptureProcessor::channelPeak() const noexcept
{
    const auto* c = control();
    return c != nullptr ? c->channelPeak.load(std::memory_order_relaxed) : 0.0f;
}

float CaptureProcessor::outputPeak() const noexcept
{
    return outputPeakValue.load(std::memory_order_relaxed);
}

uint32_t CaptureProcessor::tapRate() const noexcept
{
    const auto* c = control();
    return c != nullptr ? c->tapRate.load(std::memory_order_relaxed) : 0u;
}

uint32_t CaptureProcessor::channelRate() const noexcept
{
    return channelRing.isOpen() ? channelRing.sampleRate() : 0u;
}

float CaptureProcessor::tapGain() const noexcept
{
    const auto* c = control();
    return c != nullptr ? c->tapGain.load(std::memory_order_relaxed) : 1.0f;
}

int CaptureProcessor::driftPPM() const noexcept
{
    return static_cast<int>(std::lround(tapBridge.appliedRatioPPM()));
}

uint64_t CaptureProcessor::underruns() const noexcept
{
    return tapBridge.underruns() + channelBridge.underruns();
}

#ifndef WNG_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CaptureProcessor();
}
#endif
