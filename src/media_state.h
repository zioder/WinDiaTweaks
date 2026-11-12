#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace media {

// A UI reader never takes the audio worker's lock or calls an audio interface. Timestamp and level
// travel in one atomic value, so even a stuck producer cannot hold up a frame or leave it a torn sample.
class PeakMailbox {
public:
    void Request(std::uint64_t now) {
        auto until = demandUntil_.load(std::memory_order_relaxed);
        while (until < now + 500 &&
               !demandUntil_.compare_exchange_weak(until, now + 500, std::memory_order_relaxed)) {}
    }
    bool Wanted(std::uint64_t now) const {
        return now < demandUntil_.load(std::memory_order_relaxed);
    }
    void Publish(float level, std::uint64_t now) {
        unsigned value = 65535;  // unavailable
        if (std::isfinite(level) && level >= 0)
            value = static_cast<unsigned>((level > 1 ? 1 : level) * 65534);
        sample_.store((now << 16) | value, std::memory_order_relaxed);
    }
    float Read(std::uint64_t now) const {
        auto sample = sample_.load(std::memory_order_relaxed);
        auto at = sample >> 16;
        auto value = static_cast<unsigned>(sample & 65535);
        if (!sample || now < at || now - at > 250 || value == 65535) return -1;
        return value / 65534.0f;
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> sample_{0}, demandUntil_{0};
};

}  // namespace media
