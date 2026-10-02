#pragma once

#include <cstdio>
#include <cstdlib>

// OB_ASSERT checks internal invariants (things that are bugs if false, never
// bad user input; bad input becomes a Rejected event instead).
//
// Why a macro of our own instead of <cassert>: it prints the same message on
// every platform, and there is one place to change if we ever want asserts in
// an optimized build. Like assert(), it compiles to nothing under NDEBUG, so
// Release benchmarks do not pay for it.
namespace ob::detail {
[[noreturn]] inline void assert_fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "OB_ASSERT failed: %s (%s:%d)\n", expr, file, line);
    std::abort();
}
}  // namespace ob::detail

#ifdef NDEBUG
#define OB_ASSERT(cond) ((void)0)
#else
#define OB_ASSERT(cond) \
    ((cond) ? (void)0 : ::ob::detail::assert_fail(#cond, __FILE__, __LINE__))
#endif
