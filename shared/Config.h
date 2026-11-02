#pragma once

#include <cstdint>
#include <string>

namespace wng {

inline constexpr const char* kProductName        = "万能鸭录音机架";
inline constexpr const char* kProductNameShort   = "wannengya";

inline constexpr const char* kAgentBundleID      = "com.wannengya.agent";
inline constexpr const char* kAgentName          = "万能鸭录音机架";
inline constexpr const char* kAgentDaemonPath    = "/Applications/wannengya Agent.app/Contents/MacOS/wannengya Agent";

inline constexpr const char* kDriverBundleID     = "com.wannengya.hal";

// Four duplex virtual devices. Each one loops its own output stream back into
// its own input stream inside the driver, on the same sample clock, so the
// loop adds no buffering at all.
inline constexpr int kDevicePairs = 4;

inline constexpr const char* kPairUID[kDevicePairs] = {
    "com.macloopback.wannengya.12",
    "com.macloopback.wannengya.34",
    "com.macloopback.wannengya.56",
    "com.macloopback.wannengya.78",
};

inline constexpr const char* kPairName[kDevicePairs] = {
    "wannengya 1/2",
    "wannengya 3/4",
    "wannengya 5/6",
    "wannengya 7/8",
};

// Pair 1/2 is the default DAW -> OBS / 抖音 / 微信 microphone bus.
inline constexpr int kDefaultReturnPair  = 0;
// Pair 3/4 is the default capture source for external players.
inline constexpr int kDefaultCapturePair = 1;

inline constexpr uint32_t kRingChannels   = 2;
inline constexpr uint32_t kRingCapacity   = 192000; // 4 s at 48 kHz
inline constexpr uint32_t kDefaultRate    = 48000;

// Target standing buffer inside every cross-process/cross-device ring. The
// resampler holds this constant, so this is the only latency the bridge adds.
inline constexpr uint32_t kTargetFrames    = 128;
inline constexpr uint32_t kMaxTargetFrames = 2048;

// Human readable pair label, e.g. "wannengya 3/4 (in)".
inline std::string pairLabel(int index)
{
    if (index < 0 || index >= kDevicePairs)
        return "—";
    return kPairName[index];
}

inline int pairIndexForUID(const std::string& uid)
{
    for (int i = 0; i < kDevicePairs; ++i)
        if (uid == kPairUID[i])
            return i;
    return -1;
}

} // namespace wng
