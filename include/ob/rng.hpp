#pragma once

#include <cstdint>

// Our own PRNG, used by the fuzz test and the benchmark workload generators.
//
// Why not std::mt19937 + std::uniform_int_distribution: the engine (mt19937)
// is fully specified, but the *distributions* are implementation-defined. The
// same seed gives different numbers on libc++ (macOS) and libstdc++ (Linux
// CI), so a failing fuzz seed found on one machine would not reproduce on
// the other. Owning both the generator and the range mapping fixes that.
namespace ob {

// splitmix64: used only to expand one 64-bit seed into xoshiro's 256-bit
// state. Feeding a small seed (like 1, 2, 3) straight into xoshiro would give
// a mostly-zero state and poor early output.
inline std::uint64_t splitmix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// xoshiro256** (Blackman and Vigna): fast, small state, good statistical
// quality for simulation. Not cryptographic, which we do not need.
class Rng {
public:
    explicit Rng(std::uint64_t seed) {
        for (std::uint64_t& word : s_) word = splitmix64(seed);
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, n). Rejection sampling removes modulo bias: values from
    // the incomplete last block of size n are thrown away and redrawn.
    std::uint64_t below(std::uint64_t n) {
        const std::uint64_t limit = UINT64_MAX - UINT64_MAX % n;
        std::uint64_t x;
        do {
            x = next();
        } while (x >= limit);
        return x % n;
    }

    // Uniform in [lo, hi], both inclusive.
    std::int64_t uniform(std::int64_t lo, std::int64_t hi) {
        const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
        return lo + static_cast<std::int64_t>(below(span));
    }

    // True with probability pct/100. Integer percent keeps floating point out.
    bool percent(std::uint64_t pct) { return below(100) < pct; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4];
};

}  // namespace ob
