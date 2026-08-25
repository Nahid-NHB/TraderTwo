// tests/test_market_data.cpp
//
// Tests for the market-data publisher.

#include "tt/common/types.hpp"
#include "tt/market_data/publisher.hpp"
#include "tt/market_data/recorder.hpp"
#include "tt/matching/matching_engine.hpp"

#include <gtest/gtest.h>

using namespace tt;

namespace {
Price p(int64_t v) { return Price{v}; }
Quantity q(int64_t v) { return Quantity{v}; }
}

TEST(MarketData, TradesRecorded) {
    MatchingEngine eng;
    eng.register_instrument(42);

    MarketDataPublisher pub(eng);
    eng.submit_limit(42, 1, Side::Sell, p(100), q(50), TimeInForce::GTC, pub);
    eng.submit_limit(42, 2, Side::Buy,  p(100), q(20), TimeInForce::GTC, pub);

    auto tobs = pub.recent_events();
    ASSERT_FALSE(tobs.empty());
    // Two trades were generated for the buy that swept 20 against the ask.
    int trade_count = 0;
    for (const auto& e : tobs) {
        if (e.kind == EventKind::Trade) ++trade_count;
    }
    EXPECT_EQ(trade_count, 1);
}

TEST(MarketData, TopOfBookUpdatesOnTrade) {
    MatchingEngine eng;
    eng.register_instrument(42);

    MarketDataPublisher pub(eng);
    // Rest an ask, then a crossing buy at a higher price (101), filling the ask.
    eng.submit_limit(42, 1, Side::Sell, p(101), q(10), TimeInForce::GTC, pub);
    eng.submit_limit(42, 2, Side::Buy,  p(101), q(10), TimeInForce::GTC, pub);

    auto tob = pub.top_of_book(42);
    EXPECT_FALSE(tob.has_bid);
    EXPECT_FALSE(tob.has_ask);
}

TEST(MarketData, TopOfBookRestsRestingOrders) {
    MatchingEngine eng;
    eng.register_instrument(42);
    MarketDataPublisher pub(eng);

    eng.submit_limit(42, 1, Side::Sell, p(101), q(10), TimeInForce::GTC, pub);
    eng.submit_limit(42, 2, Side::Buy,  p(100), q(10), TimeInForce::GTC, pub);

    auto tob = pub.top_of_book(42);
    EXPECT_TRUE(tob.has_bid);
    EXPECT_TRUE(tob.has_ask);
    EXPECT_EQ(tob.bid_price.ticks, 100);
    EXPECT_EQ(tob.ask_price.ticks, 101);
}

TEST(MarketData, TopOfBookShowsRestingOrders) {
    MatchingEngine eng;
    eng.register_instrument(42);

    MarketDataPublisher pub(eng);
    eng.submit_limit(42, 1, Side::Sell, p(105), q(10), TimeInForce::GTC, pub);
    eng.submit_limit(42, 2, Side::Buy,  p( 95), q(20), TimeInForce::GTC, pub);

    auto tob = pub.top_of_book(42);
    EXPECT_TRUE(tob.has_bid);
    EXPECT_TRUE(tob.has_ask);
    EXPECT_EQ(tob.bid_price.ticks, 95);
    EXPECT_EQ(tob.bid_qty.qty, 20);
    EXPECT_EQ(tob.ask_price.ticks, 105);
    EXPECT_EQ(tob.ask_qty.qty, 10);
}

TEST(MarketData, ListenerReceivesEvents) {
    MatchingEngine eng;
    eng.register_instrument(42);
    MarketDataPublisher pub(eng);
    RecordingListener rec;
    pub.subscribe(&rec);

    eng.submit_limit(42, 1, Side::Sell, p(105), q(10), TimeInForce::GTC, pub);
    eng.submit_limit(42, 2, Side::Sell, p(105), q(10), TimeInForce::GTC, pub);
    eng.submit_limit(42, 3, Side::Buy,  p(105), q(15), TimeInForce::GTC, pub);

    std::size_t before = rec.events.size();
    EXPECT_GE(before, 3u);
    EXPECT_GE(rec.snapshots.size(), 1u);

    pub.unsubscribe(&rec);
    eng.submit_limit(42, 4, Side::Sell, p(110), q(5), TimeInForce::GTC, pub);
    EXPECT_EQ(rec.events.size(), before);  // unchanged after unsubscribe
}

TEST(MarketData, DepthSnapshot) {
    MatchingEngine eng;
    eng.register_instrument(42);
    MarketDataPublisher pub(eng);

    eng.submit_limit(42, 1, Side::Buy,  p(99),  q(10), TimeInForce::GTC, pub);
    eng.submit_limit(42, 2, Side::Buy,  p(100), q(20), TimeInForce::GTC, pub);
    eng.submit_limit(42, 3, Side::Sell, p(101), q(30), TimeInForce::GTC, pub);
    eng.submit_limit(42, 4, Side::Sell, p(102), q(40), TimeInForce::GTC, pub);

    auto bids = pub.bid_depth(42, 2);
    auto asks = pub.ask_depth(42, 2);

    ASSERT_EQ(bids.size(), 2u);
    ASSERT_EQ(asks.size(), 2u);

    EXPECT_EQ(bids[0].price.ticks, 100);
    EXPECT_EQ(bids[0].total_quantity.qty, 20);
    EXPECT_EQ(bids[1].price.ticks, 99);

    EXPECT_EQ(asks[0].price.ticks, 101);
    EXPECT_EQ(asks[1].price.ticks, 102);
}