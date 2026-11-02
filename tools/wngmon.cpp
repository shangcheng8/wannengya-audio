// wngmon — prints the shared-memory state of the running bridge.
//
//   wngmon            summary: agent heartbeat, selected sources, ring levels
//   wngmon -w         watch mode, one line every 500 ms
//   wngmon -r <name>  raw counters for one ring (tap | channel | return)

#include "Control.h"
#include "Config.h"
#include "RingBuffer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

const char* stateText(wng::State state)
{
    switch (state)
    {
        case wng::State::Stopped: return "stopped";
        case wng::State::Starting: return "starting";
        case wng::State::Running: return "running";
        case wng::State::Waiting: return "waiting";
        case wng::State::Error: return "error";
        case wng::State::Denied: return "denied";
    }
    return "?";
}

std::string ringNameFor(const std::string& kind)
{
    if (kind == "tap") return wng::tapRingName();
    if (kind == "channel") return wng::channelRingName();
    if (kind == "return") return wng::returnRingName();
    return kind;
}

int raw(const std::string& name)
{
    wng::RingBuffer ring;

    if (! ring.open(ringNameFor(name)))
    {
        std::fprintf(stderr, "cannot open %s\n", ringNameFor(name).c_str());
        return 1;
    }

    auto* h = ring.header();
    std::printf("ring %-28s rate=%u active=%u\n", name.c_str(), h->sampleRate.load(),
                h->active.load());
    std::printf("  frames written=%llu read=%llu available=%u\n",
                (unsigned long long) h->writeFrames.load(std::memory_order_relaxed),
                (unsigned long long) h->readFrames.load(std::memory_order_relaxed),
                ring.available());
    std::printf("  overruns=%llu underruns=%llu peak=%.1f dB source=\"%.*s\"\n",
                (unsigned long long) h->overruns.load(), (unsigned long long) h->underruns.load(),
                h->peak.load() > 0.0f ? 20.0f * std::log10(h->peak.load()) : -120.0f,
                static_cast<int>(sizeof(h->sourceName)), h->sourceName);
    return 0;
}

void summary()
{
    wng::ControlMapping mapping;
    mapping.open();

    auto* c = mapping.block();

    wng::RingBuffer tap, channel, ret;
    tap.open(wng::tapRingName());
    channel.open(wng::channelRingName());
    ret.open(wng::returnRingName());

    if (c == nullptr)
    {
        std::printf("no control block\n");
        return;
    }

    const uint64_t heartbeat = c->heartbeat.load();
    const double age = heartbeat == 0 ? -1.0 : static_cast<double>(wng::nowNs() - heartbeat) / 1e9;

    std::printf("agent    %s  (heartbeat %.1f s ago)\n", age >= 0.0 && age < 2.0 ? "online" : "offline", age);
    std::printf("mode     %u   app pid %d \"%.*s\"   capture pair %d   return pair %d\n",
                c->mode.load(), c->appPID.load(), static_cast<int>(sizeof(c->appName)), c->appName,
                c->capturePair.load(), c->returnPair.load());
    std::printf("tap      %-9s  channel %-9s  return %-9s\n",
                stateText(static_cast<wng::State>(c->tapState.load())),
                stateText(static_cast<wng::State>(c->channelState.load())),
                stateText(static_cast<wng::State>(c->returnState.load())));
    std::printf("peak     tap %6.1f dB   channel %6.1f dB   return %6.1f dB\n",
                c->tapPeak.load() > 0.0f ? 20.0f * std::log10(c->tapPeak.load()) : -120.0f,
                c->channelPeak.load() > 0.0f ? 20.0f * std::log10(c->channelPeak.load()) : -120.0f,
                c->returnPeak.load() > 0.0f ? 20.0f * std::log10(c->returnPeak.load()) : -120.0f);
    std::printf("rates    host %u Hz   tap %u Hz   channel %u Hz\n",
                c->hostRate.load(), tap.sampleRate(), channel.sampleRate());
    std::printf("buffers  tap %6u   channel %6u   return %6u\n",
                tap.available(), channel.available(), ret.available());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "-r") == 0)
        return raw(argv[2]);

    if (argc >= 2 && std::strcmp(argv[1], "-w") == 0)
    {
        while (true)
        {
            std::printf("\033[2J\033[H");
            summary();
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    summary();
    return 0;
}
