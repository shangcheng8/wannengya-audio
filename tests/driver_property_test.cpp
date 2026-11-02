// Checks that the driver's reported property sizes match what it writes.
//
// coreaudiod allocates exactly GetPropertyDataSize() bytes and the driver
// writes into that buffer. A size that is too small is a heap overflow inside
// coreaudiod: it shows up as
//
//     HALS_UCRemotePlugIn::ObjectGetPropertyDataSize: failed: ..., Error: 2003332927
//     HALS_PlugIn.cpp:1518  Caught exception trying to add device
//
// and the devices never appear. Reporting sizeof(UInt32) for the CFString
// properties (device UID, model UID) did exactly that, so every property the
// host is known to query is checked here with a guard region after the buffer.
#include "../driver/WannengyaDriver.cpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void check(AudioObjectID object, UInt32 selector, UInt32 scope, const char* name)
{
    AudioObjectPropertyAddress address { selector, static_cast<AudioObjectPropertyScope>(scope),
                                         kAudioObjectPropertyElementMain };

    if (! wng_HasProperty(gDriverRef, object, 0, &address))
        return;

    UInt32 size = 0;
    const OSStatus sizeStatus = wng_GetPropertyDataSize(gDriverRef, object, 0, &address, 0, nullptr, &size);

    if (sizeStatus != kAudioHardwareNoError)
    {
        std::printf("  FAIL  %-26s object %2u size status %d\n", name, object, (int) sizeStatus);
        ++failures;
        return;
    }

    if (size == 0)
        return;

    constexpr UInt32 guard = 64;
    std::vector<unsigned char> buffer(size + guard, 0xAB);

    UInt32 written = size;
    const OSStatus readStatus = wng_GetPropertyData(gDriverRef, object, 0, &address, 0, nullptr,
                                                    size, &written, buffer.data());

    bool overflowed = false;
    for (UInt32 i = size; i < size + guard; ++i)
        overflowed = overflowed || buffer[i] != 0xAB;

    if (readStatus != kAudioHardwareNoError || overflowed || written != size)
    {
        std::printf("  FAIL  %-26s object %2u size %u written %u status %d%s\n",
                    name, object, size, written, (int) readStatus,
                    overflowed ? " overflow" : "");
        ++failures;
    }
}

} // namespace

int main()
{
    wng_Initialize(gDriverRef, nullptr);

    struct Query { UInt32 selector; UInt32 scope; const char* name; };

    const Query pluginQueries[] = {
        { kAudioObjectPropertyBaseClass,       kAudioObjectPropertyScopeGlobal, "plug-in base class" },
        { kAudioObjectPropertyClass,           kAudioObjectPropertyScopeGlobal, "plug-in class" },
        { kAudioObjectPropertyManufacturer,    kAudioObjectPropertyScopeGlobal, "plug-in manufacturer" },
        { kAudioObjectPropertyOwnedObjects,    kAudioObjectPropertyScopeGlobal, "plug-in owned objects" },
        { kAudioPlugInPropertyDeviceList,      kAudioObjectPropertyScopeGlobal, "plug-in device list" },
        { kAudioPlugInPropertyBundleID,        kAudioObjectPropertyScopeGlobal, "plug-in bundle id" },
        { kAudioPlugInPropertyResourceBundle,  kAudioObjectPropertyScopeGlobal, "plug-in resource bundle" },
    };

    const Query deviceQueries[] = {
        { kAudioObjectPropertyBaseClass,                    kAudioObjectPropertyScopeGlobal, "device base class" },
        { kAudioObjectPropertyClass,                        kAudioObjectPropertyScopeGlobal, "device class" },
        { kAudioObjectPropertyOwner,                        kAudioObjectPropertyScopeGlobal, "device owner" },
        { kAudioObjectPropertyName,                         kAudioObjectPropertyScopeGlobal, "device name" },
        { kAudioObjectPropertyManufacturer,                 kAudioObjectPropertyScopeGlobal, "device manufacturer" },
        { kAudioObjectPropertyOwnedObjects,                 kAudioObjectPropertyScopeGlobal, "device owned objects" },
        { kAudioObjectPropertyControlList,                  kAudioObjectPropertyScopeGlobal, "device control list" },
        { kAudioDevicePropertyDeviceUID,                    kAudioObjectPropertyScopeGlobal, "device uid" },
        { kAudioDevicePropertyModelUID,                     kAudioObjectPropertyScopeGlobal, "device model uid" },
        { kAudioDevicePropertyTransportType,                kAudioObjectPropertyScopeGlobal, "device transport" },
        { kAudioDevicePropertyRelatedDevices,               kAudioObjectPropertyScopeGlobal, "device related" },
        { kAudioDevicePropertyClockDomain,                  kAudioObjectPropertyScopeGlobal, "device clock domain" },
        { kAudioDevicePropertyDeviceIsAlive,                kAudioObjectPropertyScopeGlobal, "device is alive" },
        { kAudioDevicePropertyDeviceIsRunning,              kAudioObjectPropertyScopeGlobal, "device is running" },
        { kAudioDevicePropertyDeviceIsRunningSomewhere,     kAudioObjectPropertyScopeGlobal, "device running anywhere" },
        { kAudioDevicePropertyDeviceCanBeDefaultDevice,     kAudioObjectPropertyScopeGlobal, "device can be default" },
        { kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeGlobal, "device can be system" },
        { kAudioDevicePropertyHogMode,                      kAudioObjectPropertyScopeGlobal, "device hog mode" },
        { kAudioDevicePropertyLatency,                      kAudioObjectPropertyScopeGlobal, "device latency" },
        { kAudioDevicePropertySafetyOffset,                 kAudioObjectPropertyScopeGlobal, "device safety offset" },
        { kAudioDevicePropertyNominalSampleRate,            kAudioObjectPropertyScopeGlobal, "device sample rate" },
        { kAudioDevicePropertyAvailableNominalSampleRates,  kAudioObjectPropertyScopeGlobal, "device sample rates" },
        { kAudioDevicePropertyIsHidden,                     kAudioObjectPropertyScopeGlobal, "device is hidden" },
        { kAudioDevicePropertyPreferredChannelsForStereo,   kAudioObjectPropertyScopeGlobal, "device stereo channels" },
        { kAudioDevicePropertyPreferredChannelLayout,       kAudioObjectPropertyScopeGlobal, "device channel layout" },
        { kAudioDevicePropertyZeroTimeStampPeriod,          kAudioObjectPropertyScopeGlobal, "device zero period" },
        { kAudioDevicePropertyStreams,                      kAudioObjectPropertyScopeGlobal, "device streams" },
        { kAudioDevicePropertyStreams,                      kAudioObjectPropertyScopeInput,  "device input streams" },
        { kAudioDevicePropertyStreams,                      kAudioObjectPropertyScopeOutput, "device output streams" },
        { kAudioDevicePropertyStreamConfiguration,          kAudioObjectPropertyScopeGlobal, "device stream config" },
        { kAudioDevicePropertyStreamConfiguration,          kAudioObjectPropertyScopeInput,  "device input config" },
        { kAudioDevicePropertyStreamConfiguration,          kAudioObjectPropertyScopeOutput, "device output config" },
    };

    const Query streamQueries[] = {
        { kAudioObjectPropertyBaseClass,                   kAudioObjectPropertyScopeGlobal, "stream base class" },
        { kAudioObjectPropertyClass,                       kAudioObjectPropertyScopeGlobal, "stream class" },
        { kAudioObjectPropertyOwner,                       kAudioObjectPropertyScopeGlobal, "stream owner" },
        { kAudioStreamPropertyIsActive,                    kAudioObjectPropertyScopeGlobal, "stream is active" },
        { kAudioStreamPropertyDirection,                   kAudioObjectPropertyScopeGlobal, "stream direction" },
        { kAudioStreamPropertyTerminalType,                kAudioObjectPropertyScopeGlobal, "stream terminal type" },
        { kAudioStreamPropertyStartingChannel,             kAudioObjectPropertyScopeGlobal, "stream start channel" },
        { kAudioStreamPropertyLatency,                     kAudioObjectPropertyScopeGlobal, "stream latency" },
        { kAudioStreamPropertyVirtualFormat,               kAudioObjectPropertyScopeGlobal, "stream virtual format" },
        { kAudioStreamPropertyPhysicalFormat,              kAudioObjectPropertyScopeGlobal, "stream physical format" },
        { kAudioStreamPropertyAvailableVirtualFormats,     kAudioObjectPropertyScopeGlobal, "stream virtual formats" },
        { kAudioStreamPropertyAvailablePhysicalFormats,    kAudioObjectPropertyScopeGlobal, "stream physical formats" },
    };

    for (const auto& query : pluginQueries)
        check(kObjectID_PlugIn, query.selector, query.scope, query.name);

    for (int i = 0; i < WNG_DEVICE_COUNT; ++i)
    {
        const AudioObjectID device = kObjectID_Device0 + i;

        for (const auto& query : deviceQueries)
            check(device, query.selector, query.scope, query.name);

        check(kObjectID_StreamInputBase + i, kAudioObjectPropertyOwnedObjects,
              kAudioObjectPropertyScopeGlobal, "stream owned objects");

        for (const auto& query : streamQueries)
            check(kObjectID_StreamInputBase + i, query.selector, query.scope, query.name);

        for (const auto& query : streamQueries)
            check(kObjectID_StreamOutputBase + i, query.selector, query.scope, query.name);
    }

    std::printf("property size/data mismatches: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
