// Exercises the HAL driver's IO path directly (no install needed).
// Builds the driver source directly so its IO path can be exercised without
// installing the HAL bundle. Link with: clang++ -I ../shared driver/WannengyaDriver.cpp ...
#include "../driver/WannengyaDriver.cpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static void cycle(AudioObjectID device, int index, double inTime, double outTime,
                  uint32_t frames, const float* toWrite, float* readBack)
{
    AudioServerPlugInIOCycleInfo info{};
    info.mNominalIOBufferFrameSize = frames;
    info.mInputTime.mSampleTime = inTime;
    info.mOutputTime.mSampleTime = outTime;

    if (toWrite != nullptr)
        wng_DoIOOperation(gDriverRef, device, kObjectID_StreamOutputBase + index, 0,
                          kAudioServerPlugInIOOperationWriteMix, frames, &info,
                          const_cast<float*>(toWrite), nullptr);

    if (readBack != nullptr)
        wng_DoIOOperation(gDriverRef, device, kObjectID_StreamInputBase + index, 0,
                          kAudioServerPlugInIOOperationReadInput, frames, &info,
                          readBack, nullptr);
}

int main()
{
    wng_Initialize(gDriverRef, nullptr);
    const AudioObjectID dev = kObjectID_Device0 + 1;   // pair 3/4
    wng_StartIO(gDriverRef, dev, 0);

    const uint32_t frames = 128;
    std::vector<float> out(frames * 2), in(frames * 2);

    // 1) bit-exactness and exact delay, addressed by sample time.
    int firstHeard = -1;
    uint32_t mismatches = 0;
    for (int c = 0; c < 64; ++c)
    {
        const double t = static_cast<double>(c) * frames;
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float v = (c == 8 && i == 0) ? 0.75f : static_cast<float>(c * 1000 + i) * 1.0e-4f;
            out[i * 2] = v;
            out[i * 2 + 1] = -v;
        }

        // write the current cycle, read the *previous* one (input lags output)
        cycle(dev, 1, t, t, frames, out.data(), nullptr);
        cycle(dev, 1, t, t, frames, nullptr, in.data());

        for (uint32_t i = 0; i < frames; ++i)
            if (in[i * 2] == 0.75f && firstHeard < 0)
                firstHeard = c;
    }

    // 2) realistic timing. The host presents output ahead of input, so the
    //    cycle that writes sample time t is a later callback than the one that
    //    reads sample time t. Data written at output time t must appear at
    //    input time t, one callback later.
    std::vector<float> a(frames * 2, 0.5f), c2(frames * 2, 0.25f), b(frames * 2, 0.0f);
    cycle(dev, 1, 9000.0, 9000.0, frames, a.data(), nullptr);          // cycle 1: write 9000
    cycle(dev, 1, 9128.0, 9128.0, frames, c2.data(), nullptr);         // cycle 2: write 9128
    cycle(dev, 1, 9000.0, 9128.0 + 64.0, frames, nullptr, b.data());   // cycle 3: read 9000
    for (uint32_t i = 0; i < frames * 2; ++i)
        if (b[i] != 0.5f) ++mismatches;

    printf("freestanding? impulse written at cycle 8 seen at cycle %d\n", firstHeard);
    printf("one-cycle-behind read: %u / %u samples wrong\n", mismatches, frames * 2);

    // 3) silence on a device that has never been written
    std::vector<float> cold(frames * 2, 9.0f);
    const AudioObjectID fresh = kObjectID_Device0 + 3;
    wng_StartIO(gDriverRef, fresh, 0);
    cycle(fresh, 3, 777.0, 777.0, frames, nullptr, cold.data());
    float maxCold = 0.0f;
    for (float v : cold) maxCold = std::fmax(maxCold, std::fabs(v));
    printf("unwritten pair reads back peak %.3f (expected 0)\n", maxCold);

    // 4) reported properties
    UInt32 latency = 999, safety = 999, period = 999;
    AudioObjectPropertyAddress addr{ kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(UInt32);
    wng_GetPropertyData(gDriverRef, dev, 0, &addr, 0, nullptr, size, &size, &latency);
    addr.mSelector = kAudioDevicePropertySafetyOffset;
    size = sizeof(UInt32);
    wng_GetPropertyData(gDriverRef, dev, 0, &addr, 0, nullptr, size, &size, &safety);
    addr.mSelector = kAudioDevicePropertyZeroTimeStampPeriod;
    size = sizeof(UInt32);
    wng_GetPropertyData(gDriverRef, dev, 0, &addr, 0, nullptr, size, &size, &period);
    printf("latency=%u safety=%u zeroPeriod=%u\n", latency, safety, period);

    wng_StopIO(gDriverRef, fresh, 0);
    wng_StopIO(gDriverRef, dev, 0);
    return 0;
}
