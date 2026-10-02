// ob_bench: replays a fixed op stream through one book implementation and
// prints one JSON line of results. bench/run_all.py drives it and writes
// bench/RESULTS.md; never quote a number that did not come from there.
//
//   ob_bench --mode throughput|latency|timer --book NAME --workload NAME
//            [--runs 5] [--reps R] [--trace PATH] [--submits N] [--seed S]
//            [--hist-csv PATH]
//
// Workloads:
//   py_parity : the Python engine's exact op stream (bench/py_parity/py_parity.bin)
//   shape     : same shape, 1M submits, generated from --seed
//   wide      : same shape, prices over a 20,000-tick band (sparse book)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include "ob/books.hpp"
#include "ob/op.hpp"
#include "trace.hpp"
#include "workloads.hpp"

using namespace ob;
using namespace obbench;

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

struct Args {
    std::string mode = "throughput";
    std::string book = "baseline";
    std::string workload = "py_parity";
    std::string trace = "bench/py_parity/py_parity.bin";
    std::string hist_csv;
    int runs = 5;
    int reps = 0;  // 0 = workload default
    std::uint64_t submits = 1'000'000;
    std::uint64_t seed = 1;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--mode") a.mode = v;
        else if (k == "--book") a.book = v;
        else if (k == "--workload") a.workload = v;
        else if (k == "--trace") a.trace = v;
        else if (k == "--hist-csv") a.hist_csv = v;
        else if (k == "--runs") a.runs = std::atoi(v.c_str());
        else if (k == "--reps") a.reps = std::atoi(v.c_str());
        else if (k == "--submits") a.submits = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "--seed") a.seed = std::strtoull(v.c_str(), nullptr, 10);
        else {
            std::fprintf(stderr, "unknown flag %s\n", k.c_str());
            std::exit(2);
        }
    }
    return a;
}

// Ask the OS to keep this thread on a fast core. macOS on Apple Silicon has
// no hard affinity API; a QoS class is a strong hint toward P-cores. On Linux,
// OB_BENCH_CPU=<n> pins to one core.
std::string prefer_fast_core() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return "macOS QoS USER_INTERACTIVE (hint, not a hard pin)";
#elif defined(__linux__)
    if (const char* cpu = std::getenv("OB_BENCH_CPU")) {
        cpu_set_t set;
        CPU_ZERO(&set);
        // CPU_SET takes a size_t; convert explicitly so -Wsign-conversion stays clean.
        CPU_SET(static_cast<std::size_t>(std::atoi(cpu)), &set);
        if (sched_setaffinity(0, sizeof set, &set) == 0) return std::string("pinned to cpu ") + cpu;
    }
    return "not pinned";
#else
    return "not pinned";
#endif
}

struct Workload {
    std::vector<Op> ops;
    BookConfig cfg;
    int default_reps = 1;
    std::uint64_t submits = 0;
};

Workload load(const Args& a) {
    Workload w;
    if (a.workload == "py_parity") {
        w.ops = read_trace(a.trace);
        // 13.8k ops finish in about a millisecond, too short to time alone, so
        // one "run" replays the trace many times (each on a fresh book).
        w.default_reps = 200;
    } else if (a.workload == "shape") {
        w.ops = generate_shape(ShapeParams{a.seed, a.submits, 9950, 10050});
    } else if (a.workload == "wide") {
        // Same shape, but prices spread over the whole 20,000-tick band. The
        // book becomes sparse, which is the worst case for the array ladder's
        // scan to the next non-empty level.
        w.ops = generate_shape(ShapeParams{a.seed, a.submits, 1, 20'000});
    } else {
        std::fprintf(stderr, "unknown workload %s\n", a.workload.c_str());
        std::exit(2);
    }
    // Band wider than the workload's prices, so no order is rejected for its
    // price and a flat ladder (upgrade C) is not handed an unrealistically
    // tiny array.
    w.cfg = BookConfig{1, 20'000};
    for (const Op& op : w.ops) w.submits += op.kind == OpKind::Submit ? 1 : 0;
    return w;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

std::string json_list(const std::vector<double>& v) {
    std::ostringstream os;
    os << '[';
    for (std::size_t i = 0; i < v.size(); ++i) os << (i ? "," : "") << static_cast<std::uint64_t>(v[i]);
    os << ']';
    return os.str();
}

// End state after one replay, printed so run_all.py can check every book
// (and the Python engine) ended in the same place.
template <class Book>
std::string end_state_json(const Book& book, const CountingSink& sink) {
    BookSnapshot s = book.snapshot();
    std::ostringstream os;
    os << "\"trades\":" << sink.trades << ",\"traded_qty\":" << sink.traded_qty
       << ",\"resting_orders\":" << book.resting_count() << ",\"bids\":[";
    for (std::size_t i = 0; i < s.bids.size(); ++i) {
        os << (i ? "," : "") << '[' << s.bids[i].price << ',' << s.bids[i].total << ']';
    }
    os << "],\"asks\":[";
    for (std::size_t i = 0; i < s.asks.size(); ++i) {
        os << (i ? "," : "") << '[' << s.asks[i].price << ',' << s.asks[i].total << ']';
    }
    os << ']';
    return os.str();
}

// Throughput: time whole replays with no per-op timer calls, because timer
// calls inside the loop would add their own cost to what is being measured.
// Book construction and destruction happen outside the timed region.
template <class Book>
void throughput(const Args& a, const Workload& w, const std::string& pin) {
    const int reps = a.reps > 0 ? a.reps : w.default_reps;
    std::string end_state;
    std::vector<double> orders_per_sec, ops_per_sec;
    // Run index -1 is the warmup: same work, result discarded (warms caches,
    // the branch predictor and the allocator's free lists).
    for (int run = -1; run < a.runs; ++run) {
        std::uint64_t total_ns = 0;
        for (int r = 0; r < reps; ++r) {
            auto book = std::make_unique<Book>(w.cfg);
            CountingSink sink;
            const std::uint64_t t0 = now_ns();
            for (const Op& op : w.ops) apply(*book, op, sink);
            total_ns += now_ns() - t0;
            if (run == a.runs - 1 && r == reps - 1) end_state = end_state_json(*book, sink);
        }
        if (run < 0) continue;
        const double secs = static_cast<double>(total_ns) / 1e9;
        orders_per_sec.push_back(static_cast<double>(w.submits * static_cast<std::uint64_t>(reps)) / secs);
        ops_per_sec.push_back(static_cast<double>(w.ops.size() * static_cast<std::uint64_t>(reps)) / secs);
    }
    std::printf(
        "{\"mode\":\"throughput\",\"book\":\"%s\",\"workload\":\"%s\",\"ops\":%zu,\"submits\":%llu,"
        "\"reps_per_run\":%d,\"orders_per_sec_runs\":%s,\"orders_per_sec_median\":%.0f,"
        "\"ops_per_sec_runs\":%s,\"ops_per_sec_median\":%.0f,\"pinning\":\"%s\",%s}\n",
        a.book.c_str(), a.workload.c_str(), w.ops.size(), static_cast<unsigned long long>(w.submits),
        reps, json_list(orders_per_sec).c_str(), median(orders_per_sec), json_list(ops_per_sec).c_str(),
        median(ops_per_sec), pin.c_str(), end_state.c_str());
}

// What kind of operation a latency sample belongs to, decided from the events
// it produced. Different kinds do very different work, so mixing them in one
// distribution would hide the expensive ones.
enum Kind : int { kRest, kTrade, kCancel, kModify, kReject, kKinds };
// "rejected" here is mostly cancels of orders that already filled: the
// workload cancels ids it once saw rest, as the Python workload does.
const char* kKindNames[kKinds] = {"submit_rest", "submit_trade", "cancel", "modify", "rejected"};

struct ClassifySink {
    CountingSink counts;
    bool rejected = false;
    void operator()(const Event& e) {
        counts(e);
        if (e.type == EventType::Rejected) rejected = true;
    }
};

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    // Nearest-rank: the smallest sample with at least p% of samples <= it.
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

// Latency: one timestamp pair around every op. Samples go into vectors sized
// up front, so recording does not allocate inside the timed region.
template <class Book>
void latency(const Args& a, const Workload& w, const std::string& pin) {
    const int reps = a.reps > 0 ? a.reps : std::max(1, w.default_reps / 10);
    const std::size_t max_samples = w.ops.size() * static_cast<std::size_t>(reps);
    std::vector<double> p50[kKinds], p99[kKinds], p999[kKinds], maxv[kKinds];
    std::size_t counts[kKinds] = {};
    std::vector<std::uint64_t> last_all;

    for (int run = -1; run < a.runs; ++run) {
        std::vector<std::uint64_t> samples[kKinds];
        for (auto& s : samples) s.reserve(max_samples);
        for (int r = 0; r < reps; ++r) {
            auto book = std::make_unique<Book>(w.cfg);
            for (const Op& op : w.ops) {
                ClassifySink sink;
                const std::uint64_t t0 = now_ns();
                apply(*book, op, sink);
                const std::uint64_t dt = now_ns() - t0;
                int k = kReject;
                if (!sink.rejected) {
                    if (op.kind == OpKind::Cancel) k = kCancel;
                    else if (op.kind == OpKind::Modify) k = kModify;
                    else k = sink.counts.trades > 0 ? kTrade : kRest;
                }
                samples[k].push_back(dt);
            }
        }
        if (run < 0) continue;  // warmup
        last_all.clear();
        for (int k = 0; k < kKinds; ++k) {
            std::sort(samples[k].begin(), samples[k].end());
            counts[k] = samples[k].size();
            if (samples[k].empty()) continue;
            p50[k].push_back(static_cast<double>(percentile(samples[k], 50)));
            p99[k].push_back(static_cast<double>(percentile(samples[k], 99)));
            p999[k].push_back(static_cast<double>(percentile(samples[k], 99.9)));
            maxv[k].push_back(static_cast<double>(samples[k].back()));
            last_all.insert(last_all.end(), samples[k].begin(), samples[k].end());
        }
    }

    // Optional histogram of the last run (all kinds), for the plot script.
    if (!a.hist_csv.empty()) {
        std::map<std::uint64_t, std::uint64_t> hist;
        for (std::uint64_t v : last_all) ++hist[v];
        std::ofstream out(a.hist_csv);
        out << "latency_ns,count\n";
        for (const auto& [v, c] : hist) out << v << ',' << c << '\n';
    }

    std::printf("{\"mode\":\"latency\",\"book\":\"%s\",\"workload\":\"%s\",\"reps_per_run\":%d,"
                "\"pinning\":\"%s\",\"kinds\":{",
                a.book.c_str(), a.workload.c_str(), reps, pin.c_str());
    bool first = true;
    for (int k = 0; k < kKinds; ++k) {
        if (p50[k].empty()) continue;
        std::printf("%s\"%s\":{\"samples_per_run\":%zu,\"p50_ns\":%.0f,\"p99_ns\":%.0f,"
                    "\"p999_ns\":%.0f,\"max_ns\":%.0f,\"p50_runs\":%s,\"p99_runs\":%s,\"p999_runs\":%s}",
                    first ? "" : ",", kKindNames[k], counts[k], median(p50[k]), median(p99[k]),
                    median(p999[k]), median(maxv[k]), json_list(p50[k]).c_str(),
                    json_list(p99[k]).c_str(), json_list(p999[k]).c_str());
        first = false;
    }
    std::printf("}}\n");
}

// Measure the clock instead of trusting documentation: the smallest nonzero
// step it can show (resolution) and the cost of reading it twice back to back
// (overhead, which every latency sample includes once).
void timer_info() {
    constexpr int kN = 1'000'000;
    std::vector<std::uint64_t> d(kN);
    for (int i = 0; i < kN; ++i) {
        const std::uint64_t t0 = now_ns();
        const std::uint64_t t1 = now_ns();
        d[static_cast<std::size_t>(i)] = t1 - t0;
    }
    std::uint64_t resolution = UINT64_MAX;
    for (std::uint64_t v : d) {
        if (v > 0) resolution = std::min(resolution, v);
    }
    std::sort(d.begin(), d.end());
    std::size_t zeros = static_cast<std::size_t>(std::count(d.begin(), d.end(), 0));
    // Back-to-back reads land on the same tick most of the time when the
    // tick is coarse, so also time a long loop of reads for the true cost.
    const std::uint64_t t0 = now_ns();
    std::uint64_t sink = 0;
    for (int i = 0; i < kN; ++i) sink += now_ns();
    const double per_call = static_cast<double>(now_ns() - t0) / kN;
    std::printf("{\"mode\":\"timer\",\"clock\":\"std::chrono::steady_clock\",\"min_nonzero_step_ns\":%llu,"
                "\"back_to_back_p50_ns\":%llu,\"back_to_back_zero_fraction\":%.3f,"
                "\"cost_per_read_ns\":%.1f,\"checksum\":%llu}\n",
                static_cast<unsigned long long>(resolution),
                static_cast<unsigned long long>(d[d.size() / 2]),
                static_cast<double>(zeros) / kN, per_call, static_cast<unsigned long long>(sink % 10));
}

// Calls f.template operator()<Book>() for the book named on the command line.
template <class F>
bool with_book(const std::string& name, F&& f) {
    if (name == "baseline") return f.template operator()<BaselineBook>(), true;
    if (name == "intrusive_map") return f.template operator()<IntrusiveMapBook>(), true;
    if (name == "intrusive_array") return f.template operator()<IntrusiveArrayBook>(), true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    const std::string pin = prefer_fast_core();
    if (a.mode == "timer") {
        timer_info();
        return 0;
    }
    const Workload w = load(a);
    const bool ok = with_book(a.book, [&]<class Book>() {
        if (a.mode == "latency") latency<Book>(a, w, pin);
        else throughput<Book>(a, w, pin);
    });
    if (!ok) {
        std::fprintf(stderr, "unknown book %s\n", a.book.c_str());
        return 2;
    }
    return 0;
}
