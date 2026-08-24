// tests/test_common.cpp
//
// Tests for the common value types.

#include "tt/common/types.hpp"

#include <gtest/gtest.h>

using namespace tt;

TEST(Types, PriceIsValidOnlyForNonNegativeTicks) {
    EXPECT_TRUE(Price{0}.is_valid());
    EXPECT_TRUE(Price{1'000'000}.is_valid());
    EXPECT_FALSE(Price{-1}.is_valid());
    EXPECT_FALSE(Price{kInvalidPrice}.is_valid());
}

TEST(Types, PriceDoubleRoundTrip) {
    Price p = Price::from_double(123.456789);
    EXPECT_DOUBLE_EQ(p.to_double(), 123.456789);
}

TEST(Types, QuantityArithmetic) {
    Quantity a{100};
    Quantity b{30};
    EXPECT_EQ((a - b).qty, 70);
    a -= b;
    EXPECT_EQ(a.qty, 70);
    a += b;
    EXPECT_EQ(a.qty, 100);
}

TEST(Types, QuantityIsValid) {
    EXPECT_FALSE(Quantity{0}.is_valid());
    EXPECT_TRUE(Quantity{1}.is_valid());
    EXPECT_TRUE(Quantity{1'000'000}.is_valid());
}

TEST(Types, PriceOrdering) {
    Price a{100};
    Price b{200};
    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_EQ(a, Price{100});
}
