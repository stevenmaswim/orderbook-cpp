#pragma once

// NASDAQ TotalView-ITCH 5.0 replay (upgrade G).
//
// What this is and is not: ITCH is the exchange's market-by-order OUTPUT
// feed. Every add, execution, cancel and replace in it is a decision the
// exchange already made. Replaying it rebuilds each stock's book with our data
// structures (ladder, FIFO levels, id index); it does NOT exercise our
// matching logic, because the feed tells us what matched. The book therefore
// gets a separate non-matching "replay_*" API.
//
// Wire format (NASDAQ ITCH 5.0 spec; the public sample files): a stream of
// messages, each preceded by a 2-byte big-endian length. Every message starts
// with a 1-byte type, then stock locate (2), tracking number (2), timestamp
// (6, nanoseconds since midnight). All integers are big-endian. Prices are
// 4-byte integers with 4 implied decimals, used here directly as ticks of
// $0.0001.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ob/types.hpp"

namespace ob::itch {

// Big-endian readers. Byte by byte with shifts: correct on any host byte
// order and with no alignment requirement (ITCH fields are unaligned).
inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           std::uint32_t{p[3]};
}
inline std::uint64_t be48(const std::uint8_t* p) {
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}

// Minimum lengths of the message types we read (from the spec).
inline constexpr std::size_t kLenAdd = 36;          // 'A'
inline constexpr std::size_t kLenAddMpid = 40;      // 'F'
inline constexpr std::size_t kLenExecuted = 31;     // 'E'
inline constexpr std::size_t kLenExecPrice = 36;    // 'C'
inline constexpr std::size_t kLenCancel = 23;       // 'X'
inline constexpr std::size_t kLenDelete = 19;       // 'D'
inline constexpr std::size_t kLenReplace = 35;      // 'U'
inline constexpr std::size_t kLenTrade = 44;        // 'P'

// Calls f(message, length) for every complete length-prefixed message and
// returns the number of bytes consumed. A truncated message at the end (our
// sample is a prefix cut from a larger file) is left unconsumed.
template <class F>
std::size_t for_each_message(const std::uint8_t* data, std::size_t size, F&& f) {
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::size_t len = be16(data + pos);
        if (len == 0 || pos + 2 + len > size) break;
        f(data + pos + 2, len);
        pos += 2 + len;
    }
    return pos;
}

struct Stats {
    std::array<std::uint64_t, 256> by_type{};  // every message, by type byte
    std::uint64_t applied = 0;                 // book-changing messages applied
    std::uint64_t rejected_by_book = 0;        // e.g. execution of an unknown order ref
    std::uint64_t malformed = 0;               // shorter than its type requires
};

// Routes book-changing messages to one book per stock locate, created on
// first use. Book must provide replay_add / replay_reduce / replay_delete /
// replay_replace.
template <class Book>
class Replayer {
public:
    explicit Replayer(BookConfig cfg) : cfg_(cfg), books_(65536) {}

    void on_message(const std::uint8_t* m, std::size_t len) {
        const std::uint8_t type = m[0];
        ++stats_.by_type[type];
        bool ok = true;
        switch (type) {
            case 'A':
            case 'F': {
                if (len < (type == 'A' ? kLenAdd : kLenAddMpid)) return malformed();
                const Side side = m[19] == 'B' ? Side::Buy : Side::Sell;
                ok = book(be16(m + 1)).replay_add(be64(m + 11), side, Price{be32(m + 32)}, Qty{be32(m + 20)});
                break;
            }
            case 'E':
            case 'C':
                // 'C' also carries an execution price that can differ from the
                // resting price (e.g. a cross); the book only needs the shares.
                if (len < (type == 'E' ? kLenExecuted : kLenExecPrice)) return malformed();
                ok = book(be16(m + 1)).replay_reduce(be64(m + 11), Qty{be32(m + 19)});
                break;
            case 'X':
                if (len < kLenCancel) return malformed();
                ok = book(be16(m + 1)).replay_reduce(be64(m + 11), Qty{be32(m + 19)});
                break;
            case 'D':
                if (len < kLenDelete) return malformed();
                ok = book(be16(m + 1)).replay_delete(be64(m + 11));
                break;
            case 'U':
                if (len < kLenReplace) return malformed();
                ok = book(be16(m + 1)).replay_replace(be64(m + 11), be64(m + 19), Price{be32(m + 31)},
                                                      Qty{be32(m + 27)});
                break;
            case 'P':
                // Trade against a non-displayed order: it was never in the
                // visible book, so the book does not change. Counted only.
                if (len < kLenTrade) return malformed();
                return;
            default:
                return;  // system, directory, auction and other messages
        }
        if (ok) ++stats_.applied;
        else ++stats_.rejected_by_book;
    }

    const Stats& stats() const { return stats_; }

    // Totals across every stock's book.
    std::size_t resting_orders() const {
        std::size_t n = 0;
        for (const auto& b : books_) {
            if (b) n += b->resting_count();
        }
        return n;
    }
    std::size_t active_books() const {
        std::size_t n = 0;
        for (const auto& b : books_) {
            if (b) ++n;
        }
        return n;
    }
    const Book* find_book(std::uint16_t locate) const { return books_[locate].get(); }

private:
    Book& book(std::uint16_t locate) {
        std::unique_ptr<Book>& b = books_[locate];
        if (!b) b = std::make_unique<Book>(cfg_);
        return *b;
    }
    void malformed() { ++stats_.malformed; }

    BookConfig cfg_;
    std::vector<std::unique_ptr<Book>> books_;  // index = stock locate
    Stats stats_;
};

}  // namespace ob::itch
