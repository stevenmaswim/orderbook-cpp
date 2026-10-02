// Smoke test: proves the toolchain, gtest and the include path are wired up
// before any engine code exists.
#include <gtest/gtest.h>

#include "ob/assert.hpp"

TEST(Smoke, AssertPassesOnTrueCondition) {
    OB_ASSERT(1 + 1 == 2);
    SUCCEED();
}

#ifndef NDEBUG
// Only meaningful when asserts are compiled in (Debug and sanitizer builds).
TEST(SmokeDeathTest, AssertAbortsOnFalseCondition) {
    EXPECT_DEATH(OB_ASSERT(1 + 1 == 3), "OB_ASSERT failed");
}
#endif
