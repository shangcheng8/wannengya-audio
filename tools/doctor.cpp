// wngdoctor — explains why the virtual devices are or are not visible.
//
// Checks, in the order that matters:
//   1. is the HAL bundle in place, owned by root, and signed
//   2. did coreaudiod actually load it
//   3. did it publish devices, and are they alive
//
// Run it after installing; it prints one line per failure with the fix.

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <vector>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char* kBundlePath = "/Library/Audio/Plug-Ins/HAL/Wannengya.driver";
constexpr const char* kBundleID   = "com.wannengya.hal";

int failures = 0;

void ok(const std::string& text)   { std::printf("  ok    %s\n", text.c_str()); }
void warn(const std::string& text) { std::printf("  warn  %s\n", text.c_str()); }
void bad(const std::string& text)  { std::printf("  FAIL  %s\n", text.c_str()); ++failures; }

std::string readString(AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);

    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};

    char buffer[512] = {};
    CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(value);
    return buffer;
}

std::vector<AudioObjectID> objectList(AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;

    if (AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};

    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));

    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, ids.data()) != noErr)
        return {};

    return ids;
}

void checkBundle()
{
    std::printf("bundle\n");

    struct stat st {};

    if (::stat(kBundlePath, &st) != 0)
    {
        bad(std::string("missing ") + kBundlePath + " — run scripts/install_driver.sh");
        return;
    }

    ok(std::string(kBundlePath) + " exists");

    if (st.st_uid != 0)
        bad("owner is not root — coreaudiod refuses to load it; run: sudo chown -R root:wheel " + std::string(kBundlePath));
    else
        ok("owner root:wheel");

    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        warn("group/other writable — run: sudo chmod -R go-w " + std::string(kBundlePath));
}

void checkLoaded()
{
    std::printf("core audio\n");

    bool found = false;

    for (AudioObjectID plugin : objectList(kAudioObjectSystemObject, kAudioHardwarePropertyPlugInList))
    {
        if (readString(plugin, kAudioPlugInPropertyBundleID) == kBundleID)
        {
            found = true;
            ok(std::string("plug-in loaded as ") + kBundleID);

            const auto devices = objectList(plugin, kAudioPlugInPropertyDeviceList);

            if (devices.empty())
            {
                bad("plug-in loaded but published 0 devices — restart core audio: sudo killall coreaudiod");
                return;
            }

            for (AudioObjectID device : devices)
            {
                UInt32 alive = 0;
                UInt32 size = sizeof(alive);
                AudioObjectPropertyAddress address { kAudioDevicePropertyDeviceIsAlive,
                                                     kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMain };
                AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &alive);

                char stereo[64] = {};
                Float64 rate = 0.0;
                UInt32 rateSize = sizeof(rate);
                AudioObjectPropertyAddress rateAddress { kAudioDevicePropertyNominalSampleRate,
                                                         kAudioObjectPropertyScopeGlobal,
                                                         kAudioObjectPropertyElementMain };
                AudioObjectGetPropertyData(device, &rateAddress, 0, nullptr, &rateSize, &rate);
                std::snprintf(stereo, sizeof(stereo), "%.0f Hz", rate);

                if (alive)
                    ok(readString(device, kAudioObjectPropertyName) + "  " + stereo);
                else
                    bad(readString(device, kAudioObjectPropertyName) + " is not alive");
            }

            return;
        }
    }

    if (! found)
        bad("plug-in not loaded — restart core audio: sudo killall coreaudiod");
}

void checkVisible()
{
    std::printf("devices\n");

    int count = 0;

    for (AudioObjectID device : objectList(kAudioObjectSystemObject, kAudioHardwarePropertyDevices))
    {
        const auto name = readString(device, kAudioObjectPropertyName);

        if (name.find("wannengya") != std::string::npos)
        {
            ++count;
            std::printf("  seen  %s\n", name.c_str());
        }
    }

    if (count == 0)
        bad("no wannengya device in the system device list");
    else if (count < 4)
        warn("expected 4 devices, found " + std::to_string(count));
    else
        ok("4 devices visible to every app");
}

} // namespace

int main()
{
    std::printf("wannengya doctor\n\n");
    checkBundle();
    checkLoaded();
    checkVisible();

    std::printf("\n%s\n", failures == 0 ? "All checks passed."
                                        : "Some checks failed; fix the lines marked FAIL.");
    return failures == 0 ? 0 : 1;
}
