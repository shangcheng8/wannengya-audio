#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "RingBuffer.h"

#include "Config.h"

namespace wng {

// One control block shared by the Agent, the Capture plug-in and the Return
// plug-in. Every field is a lock-free atomic; strings are written then published
// with a sequence counter so a reader never sees a half-written name.
struct alignas(64) ControlBlock
{
    std::atomic<uint32_t> magic { 0 };
    std::atomic<uint32_t> version { 0 };

    // ---- Capture plug-in -> Agent -------------------------------------------
    std::atomic<uint32_t> captureSeq { 0 };
    std::atomic<uint32_t> mode { 0 };          // CaptureMode
    std::atomic<int32_t>  appPID { -1 };
    std::atomic<uint32_t> appObjectID { 0 };
    std::atomic<int32_t>  capturePair { kDefaultCapturePair };
    std::atomic<uint32_t> hostRate { 0 };
    char appBundleID[kRingNameSize] = {};
    char appName[kRingNameSize] = {};

    // Plug-ins bump these from their UI timer. When a heartbeat goes stale the
    // Agent releases the tap, so a crashed DAW cannot leave an App muted.
    std::atomic<uint64_t> captureHeartbeat { 0 };
    std::atomic<uint64_t> returnHeartbeat { 0 };

    // ---- Return plug-in -> Agent -------------------------------------------
    std::atomic<uint32_t> returnSeq { 0 };
    std::atomic<int32_t>  returnPair { kDefaultReturnPair };
    std::atomic<uint32_t> returnHostRate { 0 };

    // ---- Agent -> plug-ins -------------------------------------------------
    std::atomic<uint64_t> heartbeat { 0 };
    std::atomic<uint32_t> state { 0 };         // State
    std::atomic<uint32_t> fault { 0 };         // Fault
    std::atomic<uint32_t> tapState { 0 };      // State
    std::atomic<uint32_t> tapFault { 0 };
    std::atomic<uint32_t> channelState { 0 };
    std::atomic<uint32_t> channelFault { 0 };
    std::atomic<uint32_t> returnState { 0 };
    std::atomic<uint32_t> returnFault { 0 };
    std::atomic<uint32_t> permissionRaw { 0xFFFFFFFFu };
    std::atomic<uint32_t> microphoneRaw { 0xFFFFFFFFu };
    std::atomic<uint32_t> permissionRequestSeq { 0 };
    std::atomic<uint32_t> tapRate { 0 };
    std::atomic<float>    tapGain { 1.0f };    // inverse of the tap headroom loss
    std::atomic<float>    tapPeak { 0.0f };
    std::atomic<float>    channelPeak { 0.0f };
    std::atomic<float>    returnPeak { 0.0f };
    std::atomic<uint32_t> returnActive { 0 };
};

enum class CaptureMode : uint32_t { Off = 0, App = 1, Channel = 2, Both = 3 };

// Shared-memory control block mapping. Both the Agent and the plug-ins use it;
// the first process to run creates the object 0600 and owns its lifetime.
class ControlMapping
{
public:
    ControlMapping() = default;
    ~ControlMapping();

    ControlMapping(const ControlMapping&) = delete;
    ControlMapping& operator= (const ControlMapping&) = delete;

    bool open();
    void close();
    ControlBlock* block() const noexcept { return block_; }

private:
    int fd_ = -1;
    void* base_ = nullptr;
    size_t size_ = 0;
    ControlBlock* block_ = nullptr;
};

// shm_open names are per-machine, so the uid is part of every name and each
// object is created mode 0600: no other local account can read or inject audio.
inline std::string controlName()
{
    return "/wannengya." + std::to_string(static_cast<unsigned>(getuid())) + ".ctl";
}

inline std::string tapRingName()
{
    return "/wannengya." + std::to_string(static_cast<unsigned>(getuid())) + ".tap";
}

inline std::string channelRingName()
{
    return "/wannengya." + std::to_string(static_cast<unsigned>(getuid())) + ".ch";
}

inline std::string returnRingName()
{
    return "/wannengya." + std::to_string(static_cast<unsigned>(getuid())) + ".ret";
}

inline void copyText(char* destination, size_t size, const std::string& text)
{
    if (destination == nullptr || size == 0)
        return;

    std::memset(destination, 0, size);
    std::memcpy(destination, text.data(), std::min(text.size(), size - 1));
}

inline ControlMapping::~ControlMapping()
{
    close();
}

inline bool ControlMapping::open()
{
    if (block_ != nullptr)
        return true;

    size_ = sizeof(ControlBlock);
    const std::string name = controlName();

    fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    const bool fresh = fd_ >= 0;

    if (fresh)
    {
        ::fchmod(fd_, 0600);
        ::ftruncate(fd_, static_cast<off_t>(size_));
    }
    else
    {
        fd_ = ::shm_open(name.c_str(), O_RDWR, 0600);
    }

    if (fd_ < 0)
        return false;

    base_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);

    if (base_ == MAP_FAILED)
    {
        base_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    block_ = static_cast<ControlBlock*>(base_);

    // The version check matters: a control block left over from an older build
    // has different field offsets, and reinterpreting it yields garbage.
    const bool wrongVersion = block_->version.load(std::memory_order_acquire) != kRingVersion;

    if (fresh || wrongVersion
        || block_->magic.load(std::memory_order_acquire) != kRingMagic)
    {
        new (block_) ControlBlock();
        block_->version.store(kRingVersion, std::memory_order_relaxed);
        block_->capturePair.store(kDefaultCapturePair, std::memory_order_relaxed);
        block_->returnPair.store(kDefaultReturnPair, std::memory_order_relaxed);
        block_->magic.store(kRingMagic, std::memory_order_release);
    }

    return true;
}

inline void ControlMapping::close()
{
    if (base_ != nullptr)
        ::munmap(base_, size_);

    if (fd_ >= 0)
        ::close(fd_);

    base_ = nullptr;
    block_ = nullptr;
    fd_ = -1;
    size_ = 0;
}

} // namespace wng
