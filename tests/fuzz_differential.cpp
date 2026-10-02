// Differential fuzz: thousands of seeded random operation streams, each
// applied to a real book and to the reference model. After EVERY operation:
//   (a) both must have emitted identical event streams,
//   (b) both books must have identical snapshots, and
//   (c) the invariant checker must pass on the real book.
//
// Size is set by environment variables so ctest stays fast under ASan while a
// long sweep is one command away:
//   OB_FUZZ_SEEDS (default 500), OB_FUZZ_OPS (default 1000),
//   OB_FUZZ_FIRST_SEED (default 1)
// A failure prints the seed and op index; rerun just that seed with
//   OB_FUZZ_FIRST_SEED=<seed> OB_FUZZ_SEEDS=1
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

#include "book_types.hpp"
#include "invariants.hpp"
#include "ob/op.hpp"
#include "ob/rng.hpp"
#include "reference_book.hpp"

using namespace obtest;

namespace {

// A narrow band (11 ticks) so most orders cross something: matching paths get
// exercised far more often than with realistic, mostly passive flow.
constexpr Price kLo = 995;
constexpr Price kHi = 1005;
const BookConfig kFuzzConfig{kLo, kHi};

std::uint64_t env_or(const char* name, std::uint64_t fallback) {
    const char* v = std::getenv(name);
    return v ? std::strtoull(v, nullptr, 10) : fallback;
}

// Generates operations skewed toward the edge cases: reused and unknown ids,
// cancels and modifies of orders that may already be gone, FOK sizes near the
// available liquidity, and occasional invalid input.
class OpGenerator {
public:
    explicit OpGenerator(std::uint64_t seed) : rng_(seed) {}

    Op next() {
        const std::uint64_t r = rng_.below(100);
        if (r < 55 || ids_.empty()) return submit();
        if (r < 75) return cancel_op(pick_id());
        return modify();
    }

private:
    // Mostly recent ids (likely still resting), sometimes any id ever used
    // (likely dead), sometimes one that was never used.
    OrderId pick_id() {
        if (rng_.percent(5)) return 1'000'000'000 + rng_.below(1000);
        const std::size_t n = ids_.size();
        if (rng_.percent(80)) {
            const std::size_t window = n < 30 ? n : 30;
            return ids_[n - 1 - rng_.below(window)];
        }
        return ids_[rng_.below(n)];
    }

    Price price() {
        // 2% slightly outside the band to exercise InvalidPrice.
        if (rng_.percent(2)) return rng_.uniform(kLo - 3, kHi + 3);
        return rng_.uniform(kLo, kHi);
    }

    Qty qty(Qty max) {
        if (rng_.percent(1)) {
            constexpr std::array<Qty, 3> bad{0, -1, kMaxQty + 1};
            return bad[rng_.below(bad.size())];
        }
        return rng_.uniform(1, max);
    }

    Op submit() {
        NewOrder o;
        // 10% reuse an existing id: DuplicateId if it is live, legal reuse if not.
        if (!ids_.empty() && rng_.percent(10)) {
            o.id = pick_id();
        } else {
            o.id = next_id_++;
            ids_.push_back(o.id);
        }
        o.side = rng_.percent(50) ? Side::Buy : Side::Sell;
        const std::uint64_t t = rng_.below(100);
        o.type = t < 55 ? OrderType::Limit : t < 65 ? OrderType::Market
               : t < 80 ? OrderType::IOC : OrderType::FOK;
        if (o.type == OrderType::Market) {
            o.price = rng_.percent(3) ? price() : kNoPrice;  // 3% invalid priced market
        } else {
            o.price = price();
        }
        // FOK gets larger sizes so it lands on both sides of "fillable".
        o.qty = qty(o.type == OrderType::FOK ? 40 : 20);
        return submit_op(o);
    }

    Op modify() {
        const OrderId id = pick_id();
        // Half the time keep a plausible current price so in-place shrinks
        // (KeptPriority) actually happen; otherwise move it.
        const Price px = rng_.percent(50) ? rng_.uniform(kLo, kHi) : price();
        return modify_op(id, px, qty(20));
    }

    Rng rng_;
    std::vector<OrderId> ids_;
    OrderId next_id_ = 1;
};

// Counts of what the run actually exercised, so the test can fail if the
// generator silently stops reaching an edge case.
struct Coverage {
    std::array<std::uint64_t, 16> by_reason{};
    std::uint64_t trades = 0;
    void add(const std::vector<Event>& events) {
        for (const Event& e : events) {
            ++by_reason[static_cast<std::size_t>(e.reason)];
            if (e.type == EventType::Trade) ++trades;
        }
    }
};

template <class Book>
void run_seed(std::uint64_t seed, std::uint64_t n_ops, Coverage& cov) {
    Book book{kFuzzConfig};
    ReferenceBook ref{kFuzzConfig};
    InvariantChecker checker;
    OpGenerator gen(seed);
    std::deque<Op> recent;  // last few ops, printed on failure

    for (std::uint64_t i = 0; i < n_ops; ++i) {
        const Op op = gen.next();
        recent.push_back(op);
        if (recent.size() > 12) recent.pop_front();

        VectorSink got, want;
        apply(book, op, got);
        apply(ref, op, want);

        const auto context = [&] {
            std::ostringstream os;
            os << "\nseed=" << seed << " op_index=" << i << "\nlast ops:\n";
            for (const Op& r : recent) os << "  " << r << '\n';
            return os.str();
        };
        ASSERT_EQ(got.events, want.events) << context();
        ASSERT_EQ(book.snapshot(), ref.snapshot()) << context();
        std::string err = checker.on_events(got.events);
        if (err.empty()) err = checker.check_book(book);
        ASSERT_TRUE(err.empty()) << "invariant violated: " << err << context();
        cov.add(got.events);
    }
}

}  // namespace

template <class Book>
class DifferentialFuzz : public ::testing::Test {};
TYPED_TEST_SUITE(DifferentialFuzz, RealBookTypes);

TYPED_TEST(DifferentialFuzz, MatchesReferenceAndHoldsInvariants) {
    const std::uint64_t first = env_or("OB_FUZZ_FIRST_SEED", 1);
    const std::uint64_t seeds = env_or("OB_FUZZ_SEEDS", 500);
    const std::uint64_t ops = env_or("OB_FUZZ_OPS", 1000);
    Coverage cov;
    for (std::uint64_t s = first; s < first + seeds; ++s) {
        run_seed<TypeParam>(s, ops, cov);
        if (::testing::Test::HasFatalFailure()) return;  // stop at the first bad seed
    }
    // Only meaningful for a sweep of reasonable size.
    if (seeds * ops < 100'000) return;
    EXPECT_GT(cov.trades, 0u);
    for (Reason r : {Reason::InvalidQty, Reason::InvalidPrice, Reason::DuplicateId,
                     Reason::UnknownOrder, Reason::UserCancel, Reason::MarketRemainder,
                     Reason::IocRemainder, Reason::FokUnfillable, Reason::KeptPriority,
                     Reason::LostPriority}) {
        EXPECT_GT(cov.by_reason[static_cast<std::size_t>(r)], 0u)
            << "fuzz never produced " << to_string(r);
    }
}
