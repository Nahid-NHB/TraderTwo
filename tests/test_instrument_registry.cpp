// tests/test_instrument_registry.cpp
//
// Tests for the instrument registry and tick/lot validation.

#include "tt/common/instrument.hpp"
#include "tt/common/types.hpp"
#include "tt/matching/matching_engine.hpp"

#include <gtest/gtest.h>

using namespace tt;

namespace {
constexpr Price p(int64_t v) { return Price{v}; }
constexpr Quantity q(int64_t v) { return Quantity{v}; }
}  // namespace

TEST(InstrumentRegistry, RegisterAndLookup) {
    MatchingEngine eng;

    InstrumentDescriptor aapl{};
    aapl.id          = 1;
    aapl.symbol      = "AAPL";
    aapl.display_name= "Apple Inc.";
    aapl.tick_size   = p(1);
    aapl.lot_size    = q(1);
    eng.register_instrument(aapl);

    const auto* d = eng.descriptor(1);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->symbol, "AAPL");
    EXPECT_EQ(d->display_name, "Apple Inc.");
    EXPECT_EQ(d->tick_size.ticks, 1);
}

TEST(InstrumentRegistry, ValidatePriceOnTick) {
    MatchingEngine eng;
    InstrumentDescriptor d{};
    d.id        = 1;
    d.symbol    = "X";
    d.tick_size = p(5);    // ticks of 5
    d.lot_size  = q(10);   // lots of 10
    eng.register_instrument(d);

    EXPECT_EQ(eng.validate_order(1, p(100), q(100)), "");
    EXPECT_EQ(eng.validate_order(1, p(103), q(100)), "price not on tick");
    EXPECT_EQ(eng.validate_order(1, p(100), q(7)),   "qty not on lot");
    EXPECT_EQ(eng.validate_order(1, p(0),   q(100)), "");
}

TEST(InstrumentRegistry, ValidateUnknownInstrument) {
    MatchingEngine eng;
    EXPECT_EQ(eng.validate_order(99, p(100), q(100)), "instrument not registered");
}

TEST(InstrumentRegistry, UnregisterRemovesBookAndDescriptor) {
    MatchingEngine eng;
    eng.register_instrument(1);
    ASSERT_NE(eng.descriptor(1), nullptr);
    EXPECT_TRUE(eng.unregister_instrument(1));
    EXPECT_EQ(eng.descriptor(1), nullptr);
    EXPECT_EQ(eng.book(1), nullptr);
}

TEST(InstrumentRegistry, InstrumentIdsReturned) {
    MatchingEngine eng;
    eng.register_instrument(1);
    eng.register_instrument(2);
    eng.register_instrument(3);

    auto ids = eng.instrument_ids();
    EXPECT_EQ(ids.size(), 3u);
}

TEST(InstrumentTypes, RoundToTick) {
    EXPECT_EQ(round_to_tick(p(103), p(5)).ticks, 100);
    EXPECT_EQ(round_to_tick(p(100), p(5)).ticks, 100);
    EXPECT_EQ(round_to_tick(p(105), p(5)).ticks, 105);
}

TEST(InstrumentTypes, RoundToLot) {
    EXPECT_EQ(round_to_lot(q(107), q(10)).qty, 100);
    EXPECT_EQ(round_to_lot(q(100), q(10)).qty, 100);
    EXPECT_EQ(round_to_lot(q(110), q(10)).qty, 110);
}