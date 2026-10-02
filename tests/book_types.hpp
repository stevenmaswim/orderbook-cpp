#pragma once

// The list of real (non-reference) book implementations under test. Every
// new book variant gets added here and is then covered by the unit suite and
// the differential fuzz with no other test changes.
#include <gtest/gtest.h>

#include "ob/baseline_book.hpp"
#include "reference_book.hpp"

namespace obtest {

using RealBookTypes = ::testing::Types<ob::BaselineBook>;

// Real books plus the reference model. The unit suite runs over this list, which
// checks that the oracle the differential fuzz trusts agrees with every
// hand-written case.
using AllBookTypes = ::testing::Types<ob::BaselineBook, ReferenceBook>;

}  // namespace obtest
