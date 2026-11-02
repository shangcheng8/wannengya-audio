#include "Core.h"

#include "AudioDevices.h"
#include "Control.h"
#include "Config.h"
#include "DriftBridge.h"
#include "Permissions.h"
#include "ProcessList.h"
#include "ProcessTap.h"
#include "RingPlayer.h"
#include "RingRecorder.h"
#include "SystemCompat.h"

#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr uint64_t kHeartbeatTimeout = 2'000'000'000ull;
constexpr uint64_t kRetryInterval = 2'000'000'000ull;

// A plug-in that is unloaded, or a DAW that has quit, stops beating. The shared
// block outlives both, so a stale beat means "nobody wants this path" - not
// "retry forever".
constexpr uint64_t kPluginTimeout = 5'000'000'000ull;

// The control block is a second shm object; it holds no audio, only atomics.
struct ControlMapping
{
    int fd = -1;
    void* base = nullptr;
    size_t size = 0;
    wng::ControlBlock* block = nullptr;

    bool open()
    {
        if (block != nullptr)
            return true;

        size = sizeof(wng::ControlBlock);
        const std::string name = wng::controlName();

        fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);

        if (fd >= 0)
        {
            ::fchmod(fd, 0600);
            ::ftruncate(fd, static_cast<off_t>(size));
        }
        else
        {
            fd = ::shm_open(name.c_str(), O_RDWR, 0600);
        }

        if (fd < 0)
            return false;

        base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        if (base == MAP_FAILED)
        {
            base = nullptr;
            ::close(fd);
            fd = -1;
            return false;
        }

        block = static_cast<wng::ControlBlock*>(base);

        if (block->magic.load(std::memory_order_acquire) != wng::kRingMagic)
        {
            new (block) wng::ControlBlock();
            block->version.store(wng::kRingVersion, std::memory_order_relaxed);
            block->magic.store(wng::kRingMagic, std::memory_order_release);
        }

        return true;
    }
};

std::string pairUID(int index)
{
    return index >= 0 && index < wng::kDevicePairs ? wng::kPairUID[index] : std::string();
}

std::string pairInputUID(int index)
{
    // The driver exposes one duplex device per pair, so the input and output of
    // a pair share the same UID; "in" is just the input scope of that device.
    return pairUID(index);
}

} // namespace

struct Core::Impl
{
    ControlMapping control;
    wng::RingBuffer tapRing;
    wng::RingBuffer channelRing;
    wng::RingBuffer returnRing;

    std::unique_ptr<ProcessTap> tap;
    std::unique_ptr<RingRecorder> channelReader;
    std::unique_ptr<RingPlayer> returnPlayer;

    uint32_t appliedCaptureSeq = 0;
    uint32_t appliedReturnSeq = 0;
    uint32_t appliedObjectID = 0;
    int32_t appliedPID = 0;
    int32_t appliedCapturePair = -1;
    int32_t appliedReturnPair = -1;
    uint32_t appliedCaptureRate = 0;
    uint32_t appliedReturnRate = 0;

    uint64_t lastHeartbeat = 0;
    uint64_t lastTapAttempt = 0;
    uint64_t lastChannelAttempt = 0;
    uint64_t lastReturnAttempt = 0;
    uint64_t lastPermissionCheck = 0;
    uint64_t lastRateCheck = 0;

    bool micPrompted = false;
    bool tapPrompted = false;
    int lastMicRaw = -1;

    wng::ControlBlock* ctl() const { return control.block; }

    void publish(wng::State state, wng::Fault fault)
    {
        if (auto* c = ctl())
        {
            c->state.store(static_cast<uint32_t>(state), std::memory_order_release);
            c->fault.store(static_cast<uint32_t>(fault), std::memory_order_release);
        }
    }

    void publishSource(std::atomic<uint32_t>& stateField, std::atomic<uint32_t>& faultField,
                       wng::State state, wng::Fault fault)
    {
        stateField.store(static_cast<uint32_t>(state), std::memory_order_release);
        faultField.store(static_cast<uint32_t>(fault), std::memory_order_release);
    }

    bool openRings()
    {
        bool ok = true;

        if (! tapRing.isOpen())
        {
            ok = tapRing.open(wng::tapRingName())
                 || tapRing.create(wng::tapRingName(), wng::kDefaultRate);
        }

        if (! channelRing.isOpen())
        {
            ok = (channelRing.open(wng::channelRingName())
                  || channelRing.create(wng::channelRingName(), wng::kDefaultRate))
                 && ok;
        }

        if (! returnRing.isOpen())
        {
            ok = (returnRing.open(wng::returnRingName())
                  || returnRing.create(wng::returnRingName(), wng::kDefaultRate))
                 && ok;
        }

        return ok && tapRing.isOpen() && channelRing.isOpen() && returnRing.isOpen();
    }

    wng::CaptureMode mode() const
    {
        const auto raw = ctl()->mode.load(std::memory_order_relaxed);
        return raw <= 3 ? static_cast<wng::CaptureMode>(raw) : wng::CaptureMode::Off;
    }

    void applyTap()
    {
        auto* c = ctl();
        const bool wantApp = mode() == wng::CaptureMode::App || mode() == wng::CaptureMode::Both;
        const int32_t pid = c->appPID.load(std::memory_order_relaxed);
        const uint32_t hostRate = c->hostRate.load(std::memory_order_relaxed);

        if (! wantApp || pid <= 0)
        {
            tap.reset();
            appliedObjectID = 0;
            appliedPID = 0;
            publishSource(c->tapState, c->tapFault, wng::State::Stopped, wng::Fault::None);
            return;
        }

        // macOS 12 and 13 have no process taps. Keep the state honest instead of
        // retrying an API that cannot exist there: the plug-in shows 不支持 and
        // the user can still run 虚拟通道.
        if (! wng::systemSupportsAppCapture())
        {
            if (tap != nullptr || c->tapState.load(std::memory_order_relaxed)
                                      != static_cast<uint32_t>(wng::State::Denied))
            {
                NSLog(@"wannengya: App capture needs macOS 14.2 or newer (running %s)",
                      wng::systemVersionText().c_str());
            }

            tap.reset();
            appliedObjectID = 0;
            appliedPID = 0;
            publishSource(c->tapState, c->tapFault, wng::State::Denied, wng::Fault::Unsupported);
            return;
        }

        const uint32_t object = processObjectForPID(pid);

        if (object == 0)
        {
            tap.reset();
            appliedObjectID = 0;
            appliedPID = 0;
            lastTapAttempt = wng::nowNs();
            publishSource(c->tapState, c->tapFault, wng::State::Waiting, wng::Fault::ProcessGone);
            return;
        }

        const bool rateMatches = hostRate == 0
                                 || (tap != nullptr
                                     && std::abs(tap->sampleRate() - static_cast<double>(hostRate)) <= 0.5);

        if (tap && appliedObjectID == object && appliedPID == pid && rateMatches)
        {
            publishSource(c->tapState, c->tapFault, wng::State::Running, wng::Fault::None);
            return;
        }

        if (tapRing.sampleRate() != 0
            && hostRate != 0
            && tapRing.sampleRate() != hostRate
            && tap == nullptr)
        {
            tapRing.setSampleRate(hostRate);
        }

        tap.reset();
        lastTapAttempt = wng::nowNs();
        publishSource(c->tapState, c->tapFault, wng::State::Starting, wng::Fault::None);

        auto candidate = std::make_unique<ProcessTap>();
        std::string error;
        const std::string name = c->appName[0] != '\0' ? std::string(c->appName) : "App";

        if (! candidate->start(object, tapRing, name, error))
        {
            NSLog(@"wannengya: tap failed: %s", error.c_str());
            const bool gone = error.find("object") != std::string::npos;
            publishSource(c->tapState, c->tapFault,
                          gone ? wng::State::Waiting : wng::State::Error,
                          gone ? wng::Fault::ProcessGone : wng::Fault::TapFailed);
            return;
        }

        tap = std::move(candidate);
        appliedObjectID = object;
        appliedPID = pid;
        appliedCaptureRate = static_cast<uint32_t>(std::llround(tap->sampleRate()));
        publishSource(c->tapState, c->tapFault, wng::State::Running, wng::Fault::None);
        NSLog(@"wannengya: tapping %s (pid %d, %u ch @ %.0f Hz)", name.c_str(), (int) pid,
              tap->tapChannels(), tap->sampleRate());
    }

    void applyChannel()
    {
        auto* c = ctl();
        const bool wantChannel = mode() == wng::CaptureMode::Channel || mode() == wng::CaptureMode::Both;
        const int32_t rawPair = c->capturePair.load(std::memory_order_relaxed);
        const int32_t pair = rawPair >= 0 && rawPair < wng::kDevicePairs ? rawPair
                                                                        : wng::kDefaultCapturePair;
        const uint32_t hostRate = c->hostRate.load(std::memory_order_relaxed);

        if (! wantChannel || pair < 0 || pair >= wng::kDevicePairs)
        {
            channelReader.reset();
            appliedCapturePair = -1;
            publishSource(c->channelState, c->channelFault, wng::State::Stopped, wng::Fault::None);
            return;
        }

        const bool rateMatches = hostRate == 0
                                 || (channelReader != nullptr
                                     && std::abs(channelReader->sampleRate() - hostRate) <= 0.5);

        if (channelReader && appliedCapturePair == pair && rateMatches)
        {
            publishSource(c->channelState, c->channelFault, wng::State::Running, wng::Fault::None);
            return;
        }

        channelReader.reset();
        appliedCapturePair = -1;
        lastChannelAttempt = wng::nowNs();
        publishSource(c->channelState, c->channelFault, wng::State::Starting, wng::Fault::None);

        if (hostRate != 0)
            channelRing.setSampleRate(hostRate);

        auto candidate = std::make_unique<RingRecorder>();
        std::string error;
        const std::string name = std::string(wng::kPairName[pair]) + " 输入";

        if (! candidate->start(pairInputUID(pair), channelRing, name, error))
        {
            NSLog(@"wannengya: virtual input failed: %s", error.c_str());
            const bool missing = error.find("not found") != std::string::npos;

            // RingRecorder reports the raw OSStatus. -12660 is
            // kAudioHardwareSecuritySessionNotPermitted (no microphone grant);
            // the HAL device still opens, it just delivers silence.
            const bool denied = error.find("-12660") != std::string::npos;

            publishSource(c->channelState, c->channelFault,
                          missing ? wng::State::Waiting
                                  : (denied ? wng::State::Denied : wng::State::Error),
                          missing ? wng::Fault::DeviceMissing
                                  : (denied ? wng::Fault::NoPermission : wng::Fault::InputFailed));
            return;
        }

        channelReader = std::move(candidate);
        appliedCapturePair = pair;
        publishSource(c->channelState, c->channelFault, wng::State::Running, wng::Fault::None);
        NSLog(@"wannengya: reading %s", name.c_str());
    }

    void applyReturn()
    {
        auto* c = ctl();
        const int32_t rawPair = c->returnPair.load(std::memory_order_relaxed);
        const int32_t pair = rawPair >= 0 && rawPair < wng::kDevicePairs ? rawPair
                                                                        : wng::kDefaultReturnPair;
        const uint32_t hostRate = c->returnHostRate.load(std::memory_order_relaxed);

        if (pair < 0 || pair >= wng::kDevicePairs)
        {
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, wng::State::Stopped, wng::Fault::None);
            return;
        }

        // Never write the DAW mix back into the pair Capture is reading: that
        // closes a software feedback loop around the microphone.
        const int32_t capturePair = c->capturePair.load(std::memory_order_relaxed);
        const bool captureUsesReturn = capturePair == pair
                                       && (mode() == wng::CaptureMode::Channel
                                           || mode() == wng::CaptureMode::Both);

        if (captureUsesReturn)
        {
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, wng::State::Error, wng::Fault::NoRoute);
            return;
        }

        const bool rateMatches = hostRate == 0
                                 || (returnPlayer != nullptr
                                     && std::abs(returnPlayer->sampleRate() - hostRate) <= 0.5);

        // Only run the virtual output while a Return plug-in is actually alive.
        // The control block survives the DAW, so without this check a beat left
        // behind by a previous session would restart the player every two
        // seconds and tear it down again on the next tick.
        const uint64_t beat = c->returnHeartbeat.load(std::memory_order_acquire);

        if (beat == 0 || wng::nowNs() - beat > kPluginTimeout)
        {
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, wng::State::Stopped, wng::Fault::None);
            return;
        }

        if (returnPlayer && appliedReturnPair == pair && rateMatches)
        {
            publishSource(c->returnState, c->returnFault, wng::State::Running, wng::Fault::None);
            return;
        }

        returnPlayer.reset();
        appliedReturnPair = -1;
        lastReturnAttempt = wng::nowNs();
        publishSource(c->returnState, c->returnFault, wng::State::Starting, wng::Fault::None);

        if (hostRate != 0)
            returnRing.setSampleRate(hostRate);

        auto candidate = std::make_unique<RingPlayer>();
        std::string error;

        if (! candidate->start(pairUID(pair), returnRing, error))
        {
            NSLog(@"wannengya: return output failed: %s", error.c_str());
            publishSource(c->returnState, c->returnFault, wng::State::Waiting, wng::Fault::DeviceMissing);
            return;
        }

        returnPlayer = std::move(candidate);
        appliedReturnPair = pair;
        publishSource(c->returnState, c->returnFault, wng::State::Running, wng::Fault::None);
        NSLog(@"wannengya: DAW mix -> %s", wng::kPairName[pair]);
    }

    void updatePeaks()
    {
        auto* c = ctl();

        if (tapRing.isOpen())
        {
            c->tapPeak.store(tapRing.header()->peak.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            c->tapRate.store(tapRing.sampleRate(), std::memory_order_relaxed);
        }

        if (returnRing.isOpen())
            c->returnPeak.store(returnRing.header()->peak.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);

        if (auto* h = channelRing.header())
            c->channelPeak.store(h->peak.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);

        c->returnActive.store(returnPlayer && returnPlayer->isRunning() ? 1u : 0u,
                              std::memory_order_relaxed);
    }

    void updateOverall()
    {
        auto* c = ctl();
        const auto tapState = static_cast<wng::State>(c->tapState.load(std::memory_order_relaxed));
        const auto channelState = static_cast<wng::State>(c->channelState.load(std::memory_order_relaxed));
        const auto retState = static_cast<wng::State>(c->returnState.load(std::memory_order_relaxed));

        wng::State overall = wng::State::Stopped;
        wng::Fault fault = wng::Fault::None;

        const bool anySelected = mode() != wng::CaptureMode::Off
                                 || retState != wng::State::Stopped;

        if (! anySelected)
        {
            overall = wng::State::Stopped;
        }
        else if (tapState == wng::State::Error || channelState == wng::State::Error
                 || retState == wng::State::Error)
        {
            overall = wng::State::Error;
        }
        else if (tapState == wng::State::Starting || channelState == wng::State::Starting
                 || retState == wng::State::Starting)
        {
            overall = wng::State::Starting;
        }
        else if (tapState == wng::State::Running || channelState == wng::State::Running
                 || retState == wng::State::Running)
        {
            overall = wng::State::Running;
        }
        else
        {
            overall = wng::State::Waiting;
        }

        c->state.store(static_cast<uint32_t>(overall), std::memory_order_release);
        c->fault.store(static_cast<uint32_t>(fault), std::memory_order_release);
    }

    void tick()
    {
        auto* c = ctl();

        if (c == nullptr || ! openRings())
            return;

        c->heartbeat.store(wng::nowNs(), std::memory_order_release);

        const uint64_t now = wng::nowNs();

        // A crashed or unloaded plug-in must not leave an App muted. Five
        // seconds is well beyond the plug-in's 200 ms timer.
        const uint64_t captureBeat = c->captureHeartbeat.load(std::memory_order_acquire);

        if (captureBeat != 0 && now - captureBeat > 5'000'000'000ull
            && (mode() != wng::CaptureMode::Off || tap != nullptr || channelReader != nullptr))
        {
            NSLog(@"wannengya: capture plug-in stopped; releasing sources");
            c->mode.store(static_cast<uint32_t>(wng::CaptureMode::Off), std::memory_order_relaxed);
            c->appPID.store(-1, std::memory_order_relaxed);
            c->captureSeq.fetch_add(1, std::memory_order_release);
            tap.reset();
            channelReader.reset();
        }

        const uint64_t returnBeat = c->returnHeartbeat.load(std::memory_order_acquire);
        const bool returnPluginLive = returnBeat != 0 && now - returnBeat <= kPluginTimeout;

        if (! returnPluginLive && returnPlayer != nullptr)
        {
            NSLog(@"wannengya: return plug-in stopped; releasing the virtual microphone");
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, wng::State::Stopped, wng::Fault::None);
        }

        // Permissions: cheap to read, but TCC can block while a prompt is
        // pending, so poll at most once per second.
        if (now - lastPermissionCheck > 1'000'000'000ull)
        {
            lastPermissionCheck = now;
            const int raw = audioCapturePermissionRaw();
            c->permissionRaw.store(raw < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(raw),
                                   std::memory_order_relaxed);

            const int mic = microphonePermissionRaw();
            c->microphoneRaw.store(mic < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(mic),
                                   std::memory_order_relaxed);

            // Prompt once on our own as soon as a source that needs a grant is
            // selected. Relying on the installer alone is not enough: a user
            // who skipped the installer's prompt, or installed with the DAW
            // open, would otherwise only ever see "需要系统权限".
            const auto captureMode = mode();
            const bool wantsMic = captureMode == wng::CaptureMode::Channel
                                  || captureMode == wng::CaptureMode::Both;

            if (! micPrompted && wantsMic && mic == 1)
            {
                micPrompted = true;
                NSLog(@"wannengya: asking for microphone access");
                requestMicrophonePermission([this](bool granted)
                {
                    NSLog(@"wannengya: microphone access %s", granted ? "granted" : "denied");
                    lastPermissionCheck = 0;
                });
            }

            if (! tapPrompted && (captureMode == wng::CaptureMode::App
                                  || captureMode == wng::CaptureMode::Both)
                && raw == 1)
            {
                tapPrompted = true;
                NSLog(@"wannengya: asking for system audio recording access");
                requestAudioCapturePermission([this](bool granted)
                {
                    NSLog(@"wannengya: system audio recording %s", granted ? "granted" : "denied");
                    lastPermissionCheck = 0;
                });
            }

            if (mic == 2 && lastMicRaw != 2)
            {
                // The virtual input was opened before the grant landed.
                channelReader.reset();
                appliedCapturePair = -1;
            }

            lastMicRaw = mic;
        }

        const uint32_t captureSeq = c->captureSeq.load(std::memory_order_acquire);

        if (captureSeq != appliedCaptureSeq)
        {
            appliedCaptureSeq = captureSeq;
            applyTap();
            applyChannel();
            applyReturn();
        }

        const uint32_t returnSeq = c->returnSeq.load(std::memory_order_acquire);

        if (returnSeq != appliedReturnSeq)
        {
            appliedReturnSeq = returnSeq;
            applyReturn();
        }

        // Retry a source that is not running, but never more often than every
        // two seconds: each attempt creates and destroys an aggregate device.
        const bool wantTap = mode() == wng::CaptureMode::App || mode() == wng::CaptureMode::Both;
        const bool wantChannel = mode() == wng::CaptureMode::Channel || mode() == wng::CaptureMode::Both;

        if (wantTap && ! tap && now - lastTapAttempt > kRetryInterval)
            applyTap();

        if (wantChannel && ! channelReader && now - lastChannelAttempt > kRetryInterval)
            applyChannel();

        if (! returnPlayer && returnPluginLive && now - lastReturnAttempt > kRetryInterval)
            applyReturn();

        // Process object IDs move between audio sessions; re-resolve so the tap
        // does not end up attached to a dead object.
        if (tap && now - lastRateCheck > 1'000'000'000ull)
        {
            lastRateCheck = now;
            tap->refreshGain();
        }

        updatePeaks();
        updateOverall();
    }
};

Core::Core() : impl(std::make_unique<Impl>()) {}

Core::~Core()
{
    if (impl->tap) impl->tap->stop();
    if (impl->channelReader) impl->channelReader->stop();
    if (impl->returnPlayer) impl->returnPlayer->stop();
}

bool Core::open()
{
    if (! impl->control.open())
        return false;

    auto* c = impl->ctl();

    if (! impl->openRings())
        return false;

    // Defaults: capture pair 3/4, return pair 1/2, off until the plug-in says
    // otherwise. Sequenced so a plug-in that is already running is not
    // overwritten.
    if (c->captureSeq.load(std::memory_order_acquire) == 0)
    {
        c->mode.store(static_cast<uint32_t>(wng::CaptureMode::Off), std::memory_order_relaxed);
        c->capturePair.store(wng::kDefaultCapturePair, std::memory_order_relaxed);
        c->captureSeq.store(1, std::memory_order_release);
    }

    if (c->returnSeq.load(std::memory_order_acquire) == 0)
    {
        c->returnPair.store(wng::kDefaultReturnPair, std::memory_order_relaxed);
        c->returnSeq.store(1, std::memory_order_release);
    }

    impl->appliedCaptureSeq = c->captureSeq.load(std::memory_order_acquire);
    impl->appliedReturnSeq = c->returnSeq.load(std::memory_order_acquire);

    impl->applyTap();
    impl->applyChannel();
    impl->applyReturn();
    impl->updateOverall();
    return true;
}

void Core::tick()
{
    impl->tick();
}

void Core::requestInstallPermissions()
{
    impl->micPrompted = true;
    impl->tapPrompted = true;

    requestMicrophonePermission([](bool granted) {
        NSLog(@"wannengya: microphone access %s", granted ? "granted" : "denied");
    });

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        requestAudioCapturePermission([](bool granted) {
            NSLog(@"wannengya: system audio recording %s", granted ? "granted" : "denied");
        });
    });
}

wng::ControlBlock* Core::control() const noexcept
{
    return impl->ctl();
}
