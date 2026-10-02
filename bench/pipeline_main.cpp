// ob_pipeline_bench: what does putting an SPSC ring and a second thread in
// front of the book cost? (upgrade E)
//
// A gateway thread stamps each op with the time and pushes it into the ring;
// a matcher thread pops it, applies it to a PoolArrayBook and records
// (done time - stamp). Modes:
//   saturated : the gateway pushes as fast as it can. Gives pipeline
//               throughput. Latency here is mostly time spent waiting in a
//               full queue, so it says little about the handoff itself.
//   paced     : the gateway sends one op every --interval-ns (default 1000),
//               far below capacity, so the queue stays near empty and the
//               latency is handoff + matching.
//   direct    : no ring, no second thread: timestamps around the call on one
//               thread, for comparison with paced.
// --ring padded|unpadded picks the ring layout: indices on separate 128-byte
// lines, or packed together so the two threads falsely share one line.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include "ob/books.hpp"
#include "ob/op.hpp"
#include "ob/spsc_ring.hpp"
#include "workloads.hpp"

using namespace ob;

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void prefer_fast_core() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

struct Msg {
    Op op;
    std::uint64_t stamp_ns;
};

struct Result {
    double seconds = 0;
    std::vector<std::uint64_t> latency;  // per op, sorted at the end
    std::uint64_t trades = 0;
};

constexpr std::size_t kRingSlots = 1 << 14;

template <std::size_t Align>
Result run_ring(const std::vector<Op>& ops, const BookConfig& cfg, std::uint64_t interval_ns) {
    using Ring = SpscRing<Msg, kRingSlots, Align>;
    auto ring = std::make_unique<Ring>();
    auto book = std::make_unique<PoolArrayBook>(cfg);
    Result r;
    r.latency.resize(ops.size());  // sized up front: no allocation while timing
    std::atomic<bool> matcher_ready{false};
    std::uint64_t t_done = 0;

    std::thread matcher([&] {
        prefer_fast_core();
        CountingSink sink;
        matcher_ready.store(true, std::memory_order_release);
        Msg m;
        for (std::size_t i = 0; i < ops.size();) {
            if (!ring->try_pop(m)) {
                cpu_relax();
                continue;
            }
            apply(*book, m.op, sink);
            r.latency[i++] = now_ns() - m.stamp_ns;
        }
        t_done = now_ns();
        r.trades = sink.trades;
    });

    prefer_fast_core();
    while (!matcher_ready.load(std::memory_order_acquire)) cpu_relax();
    const std::uint64_t t_start = now_ns();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (interval_ns) {
            while (now_ns() < t_start + i * interval_ns) cpu_relax();
        }
        const Msg m{ops[i], now_ns()};
        while (!ring->try_push(m)) cpu_relax();
    }
    matcher.join();  // after join, t_done and r are safe to read here
    r.seconds = static_cast<double>(t_done - t_start) / 1e9;
    std::sort(r.latency.begin(), r.latency.end());
    return r;
}

Result run_direct(const std::vector<Op>& ops, const BookConfig& cfg) {
    auto book = std::make_unique<PoolArrayBook>(cfg);
    Result r;
    r.latency.resize(ops.size());
    CountingSink sink;
    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const std::uint64_t s = now_ns();
        apply(*book, ops[i], sink);
        r.latency[i] = now_ns() - s;
    }
    r.seconds = static_cast<double>(now_ns() - t0) / 1e9;
    r.trades = sink.trades;
    std::sort(r.latency.begin(), r.latency.end());
    return r;
}

std::uint64_t pct(const std::vector<std::uint64_t>& sorted, double p) {
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "saturated", ring = "padded";
    std::uint64_t interval_ns = 1000, submits = 1'000'000, seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--mode") mode = v;
        else if (k == "--ring") ring = v;
        else if (k == "--interval-ns") interval_ns = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "--submits") submits = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "--seed") seed = std::strtoull(v.c_str(), nullptr, 10);
        else {
            std::fprintf(stderr, "unknown flag %s\n", k.c_str());
            return 2;
        }
    }
    const std::vector<Op> ops = obbench::generate_shape({seed, submits, 9950, 10050});
    const BookConfig cfg{1, 20'000, 1 << 18};

    auto once = [&] {
        if (mode == "direct") return run_direct(ops, cfg);
        const std::uint64_t pace = mode == "paced" ? interval_ns : 0;
        return ring == "unpadded" ? run_ring<alignof(std::size_t)>(ops, cfg, pace)
                                  : run_ring<kCacheLine>(ops, cfg, pace);
    };
    once();  // warmup run, discarded
    const Result r = once();
    std::printf("{\"mode\":\"%s\",\"ring\":\"%s\",\"interval_ns\":%llu,\"ops\":%zu,\"trades\":%llu,"
                "\"ops_per_sec\":%.0f,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,\"max_ns\":%llu}\n",
                mode.c_str(), mode == "direct" ? "none" : ring.c_str(),
                static_cast<unsigned long long>(mode == "paced" ? interval_ns : 0), ops.size(),
                static_cast<unsigned long long>(r.trades), static_cast<double>(ops.size()) / r.seconds,
                static_cast<unsigned long long>(pct(r.latency, 50)),
                static_cast<unsigned long long>(pct(r.latency, 99)),
                static_cast<unsigned long long>(pct(r.latency, 99.9)),
                static_cast<unsigned long long>(r.latency.back()));
    return 0;
}
