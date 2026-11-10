#include "../src/media_state.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <limits>
#include <semaphore>
#include <thread>
#include <vector>

int main() {
    media::PeakMailbox peak;
    assert(peak.Read(1000) == -1);
    assert(!peak.Wanted(1000));
    peak.Request(1000);
    assert(peak.Wanted(1499));
    assert(!peak.Wanted(1500));
    peak.Request(900);  // an older UI request cannot shorten another UI thread's lease
    assert(peak.Wanted(1499));

    peak.Publish(0.75f, 1000);
    assert(std::fabs(peak.Read(1000) - 0.75f) < 0.0001f);
    assert(peak.Read(1250) >= 0);
    assert(peak.Read(1251) == -1);  // a stalled producer expires, regardless of continuing UI demand
    peak.Request(1251);
    assert(peak.Read(1251) == -1);
    assert(peak.Read(999) == -1);
    peak.Publish(2, 2000);
    assert(peak.Read(2000) == 1);
    peak.Publish(0, 2000);
    assert(peak.Read(2000) == 0);
    peak.Publish(-1, 2000);
    assert(peak.Read(2000) == -1);
    peak.Publish(std::numeric_limits<float>::quiet_NaN(), 2000);
    assert(peak.Read(2000) == -1);

    // A deliberately blocked producer must not hold up UI reads or demand updates.
    std::binary_semaphore entered(0), release(0);
    std::thread blocked([&] {
        entered.release();
        release.acquire();
        peak.Publish(0.5f, 3000);
    });
    entered.acquire();
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        peak.Request(3000);
        assert(peak.Read(3000) == -1);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    release.release();
    blocked.join();
    assert(elapsed < std::chrono::milliseconds(250));
    assert(std::fabs(peak.Read(3000) - 0.5f) < 0.0001f);

    // Several UI threads may request audio concurrently; the furthest deadline must survive.
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) readers.emplace_back([&, t] {
        for (int i = 0; i < 10000; ++i) {
            peak.Request(100000 + t * 10000 + i);
            float value = peak.Read(3000);
            assert(value >= 0 && value <= 1);
        }
    });
    for (auto& reader : readers) reader.join();
    assert(peak.Wanted(140498));
    assert(!peak.Wanted(140499));
    // Timestamp and level are coherent even while the producer publishes. At 3500 neither of
    // these two samples is valid: one is stale, the other is from the future.
    media::PeakMailbox coherent;
    std::thread writer([&] {
        for (int i = 0; i < 100000; ++i) {
            coherent.Publish(0, 2000);
            coherent.Publish(1, 4000);
        }
    });
    for (int i = 0; i < 100000; ++i) assert(coherent.Read(3500) == -1);
    writer.join();
    std::puts("PASS: levels, stale/unavailable samples, blocked producer, concurrent demand, coherent publication");
    return 0;
}
