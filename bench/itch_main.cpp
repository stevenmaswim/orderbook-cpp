// ob_itch_bench: replay a NASDAQ ITCH 5.0 file through one book per stock
// (upgrade G). The whole file is read into memory before timing starts, so
// disk speed is not measured.
//
//   ob_itch_bench --file data/itch/sample.itch --mode throughput|latency
//
// throughput: parse + apply every message, no per-message timers.
// latency   : one timestamp pair around parse + apply of each message,
//             grouped by message kind.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include "ob/books.hpp"
#include "ob/itch.hpp"

using namespace ob;

namespace {

// A map ladder because ITCH prices span $0.0001 to $200,000 across stocks:
// a flat array per stock over that band is not practical. Heap storage
// because the number of live orders per stock is unknown up front.
using ItchBook = IntrusiveMapBook;
const BookConfig kItchConfig{1, 2'000'000'000};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t pct(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

enum Kind { kAdd, kExec, kCancel, kDelete, kReplace, kOther, kKinds };
const char* kNames[kKinds] = {"add", "execute", "cancel", "delete", "replace", "other"};

Kind kind_of(std::uint8_t t) {
    switch (t) {
        case 'A': case 'F': return kAdd;
        case 'E': case 'C': return kExec;
        case 'X': return kCancel;
        case 'D': return kDelete;
        case 'U': return kReplace;
        default: return kOther;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string file = "data/itch/sample.itch", mode = "throughput";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--file") file = v;
        else if (k == "--mode") mode = v;
    }
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s (run tools/fetch_itch_sample.sh)\n", file.c_str());
        return 2;
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Warmup pass, discarded: faults in the file buffer and warms the allocator.
    {
        auto warm = std::make_unique<itch::Replayer<ItchBook>>(kItchConfig);
        itch::for_each_message(data.data(), data.size(),
                               [&](const std::uint8_t* m, std::size_t len) { warm->on_message(m, len); });
    }

    auto rep = std::make_unique<itch::Replayer<ItchBook>>(kItchConfig);
    std::uint64_t messages = 0;
    std::size_t consumed = 0;
    std::vector<std::uint64_t> lat[kKinds];
    double seconds = 0;
    if (mode == "latency") {
        for (auto& v : lat) v.reserve(data.size() / 20);  // generous; avoids growth while timing
        consumed = itch::for_each_message(data.data(), data.size(), [&](const std::uint8_t* m, std::size_t len) {
            const std::uint64_t t0 = now_ns();
            rep->on_message(m, len);
            lat[kind_of(m[0])].push_back(now_ns() - t0);
            ++messages;
        });
    } else {
        const std::uint64_t t0 = now_ns();
        consumed = itch::for_each_message(data.data(), data.size(), [&](const std::uint8_t* m, std::size_t len) {
            rep->on_message(m, len);
            ++messages;
        });
        seconds = static_cast<double>(now_ns() - t0) / 1e9;
    }

    const itch::Stats& st = rep->stats();
    std::printf("{\"mode\":\"%s\",\"bytes\":%zu,\"consumed\":%zu,\"messages\":%llu,\"applied\":%llu,"
                "\"rejected_by_book\":%llu,\"malformed\":%llu,\"resting_orders\":%zu,\"books\":%zu",
                mode.c_str(), data.size(), consumed, static_cast<unsigned long long>(messages),
                static_cast<unsigned long long>(st.applied), static_cast<unsigned long long>(st.rejected_by_book),
                static_cast<unsigned long long>(st.malformed), rep->resting_orders(), rep->active_books());
    std::printf(",\"by_type\":{");
    bool first = true;
    for (int t = 0; t < 256; ++t) {
        if (!st.by_type[static_cast<std::size_t>(t)]) continue;
        std::printf("%s\"%c\":%llu", first ? "" : ",", static_cast<char>(t),
                    static_cast<unsigned long long>(st.by_type[static_cast<std::size_t>(t)]));
        first = false;
    }
    std::printf("}");
    if (mode == "latency") {
        std::printf(",\"kinds\":{");
        first = true;
        for (int k = 0; k < kKinds; ++k) {
            auto& v = lat[k];
            if (v.empty()) continue;
            std::sort(v.begin(), v.end());
            std::printf("%s\"%s\":{\"samples\":%zu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,\"max_ns\":%llu}",
                        first ? "" : ",", kNames[k], v.size(), static_cast<unsigned long long>(pct(v, 50)),
                        static_cast<unsigned long long>(pct(v, 99)), static_cast<unsigned long long>(pct(v, 99.9)),
                        static_cast<unsigned long long>(v.back()));
            first = false;
        }
        std::printf("}");
    } else {
        std::printf(",\"seconds\":%.6f,\"msgs_per_sec\":%.0f", seconds, static_cast<double>(messages) / seconds);
    }
    std::printf("}\n");
    return 0;
}
