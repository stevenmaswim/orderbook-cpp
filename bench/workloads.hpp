#pragma once

// Synthetic workloads with the same *shape* as the Python engine's benchmark
// (Order_book_project/demo.py::trader), generated with our own Rng so they
// reproduce on any platform:
//   - side 50/50, qty uniform 1..100
//   - 90% limit at a uniform price in [lo, hi], 10% market
//   - after each submit, with probability 15%, cancel one random order this
//     generator saw rest (it may have filled since; the cancel is then rejected,
//     exactly as in the Python workload)
//
// "Saw rest" depends on the book, so the generator runs the ops against a
// BaselineBook while recording them. The result is a fixed op list: every
// book under test then replays the identical stream.
#include <cstdint>
#include <vector>

#include "ob/baseline_book.hpp"
#include "ob/op.hpp"
#include "ob/rng.hpp"

namespace obbench {

using namespace ob;

struct ShapeParams {
    std::uint64_t seed = 1;
    std::uint64_t submits = 1'000'000;
    Price lo = 9950;   // same 101-tick band as the Python workload by default
    Price hi = 10050;
};

inline std::vector<Op> generate_shape(const ShapeParams& p) {
    BaselineBook book{BookConfig{p.lo, p.hi}};
    Rng rng(p.seed);
    std::vector<Op> ops;
    ops.reserve(p.submits + p.submits / 5);
    std::vector<OrderId> my_resting;
    OrderId next_id = 1;
    VectorSink sink;

    for (std::uint64_t i = 0; i < p.submits; ++i) {
        NewOrder o;
        o.id = next_id++;
        o.side = rng.percent(50) ? Side::Buy : Side::Sell;
        o.qty = rng.uniform(1, 100);
        if (rng.percent(90)) {
            o.type = OrderType::Limit;
            o.price = rng.uniform(p.lo, p.hi);
        } else {
            o.type = OrderType::Market;
            o.price = kNoPrice;
        }
        ops.push_back(submit_op(o));
        sink.events.clear();
        book.submit(o, sink);

        // Did it rest? Only a limit order with unfilled qty does.
        Qty filled = 0;
        for (const Event& e : sink.events) {
            if (e.type == EventType::Trade) filled += e.qty;
        }
        if (o.type == OrderType::Limit && filled < o.qty) my_resting.push_back(o.id);

        if (!my_resting.empty() && rng.percent(15)) {
            const std::size_t k = static_cast<std::size_t>(rng.below(my_resting.size()));
            const OrderId victim = my_resting[k];
            // Swap-and-pop: O(1) removal. Order in this list does not matter
            // because the next pick is uniformly random anyway.
            my_resting[k] = my_resting.back();
            my_resting.pop_back();
            ops.push_back(cancel_op(victim));
            sink.events.clear();
            book.cancel(victim, sink);
        }
    }
    return ops;
}

}  // namespace obbench
