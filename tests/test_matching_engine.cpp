// tests/test_matching_engine.cpp
//
// Comprehensive tests for the matching engine: price-time priority, partial
// fills, market orders, IOC, multi-level sweeps, no-match scenarios.

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/orderbook/order_book.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace tt;

namespace {

constexpr InstrumentId kInstr = 42;
constexpr TraderId     kTrader = 1;

Price p(int64_t v) { return Price{v}; }
Quantity q(int64_t v) { return Quantity{v}; }

}  // namespace

// ---------------------------------------------------------------------------
// Limit-to-limit exact match
// ---------------------------------------------------------------------------
TEST(MatchingEngine, LimitBuyMatchesAsksExactly) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);

    CollectingSink sink;
    // Rest an ask.
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(100), TimeInForce::GTC, sink);
    // Incoming buy at the same price.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);

    ASSERT_EQ(sink.trades.size(), 1u);
    EXPECT_EQ(sink.trades[0].price.ticks, 100);
    EXPECT_EQ(sink.trades[0].quantity.qty, 100);

    // Book should now be empty on both sides.
    EXPECT_EQ(eng.book(kInstr)->best_ask().ticks, kInvalidPrice);
    EXPECT_EQ(eng.book(kInstr)->best_bid().ticks, kInvalidPrice);

    // The last SubmitResult is the taker; status should be FullyFilled.
    ASSERT_FALSE(sink.results.empty());
    EXPECT_EQ(sink.results.back().status, SubmitStatus::FullyFilled);
}

// ---------------------------------------------------------------------------
// Partial fill: incoming qty > resting qty
// ---------------------------------------------------------------------------
TEST(MatchingEngine, PartialFillIncomingExceedsResting) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Rest ask 30.
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(30), TimeInForce::GTC, sink);
    // Buy 100 @ 100: fills 30, rests 70 at 100.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);

    ASSERT_EQ(sink.trades.size(), 1u);
    EXPECT_EQ(sink.trades[0].quantity.qty, 30);

    const SubmitResult& taker = sink.results.back();
    EXPECT_EQ(taker.status, SubmitStatus::Accepted);
    EXPECT_EQ(taker.filled_quantity.qty, 30);
    EXPECT_EQ(taker.resting_quantity.qty, 70);

    EXPECT_EQ(eng.book(kInstr)->best_bid_quantity().qty, 70);
}

// ---------------------------------------------------------------------------
// Partial fill: resting qty > incoming qty (resting partially filled)
// ---------------------------------------------------------------------------
TEST(MatchingEngine, PartialFillRestingExceedsIncoming) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Rest a 100-share ask.
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(100), TimeInForce::GTC, sink);
    // Buy 30 @ 100: fills 30, no residual.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(30), TimeInForce::GTC, sink);

    ASSERT_EQ(sink.trades.size(), 1u);
    EXPECT_EQ(sink.trades[0].quantity.qty, 30);

    const SubmitResult& taker = sink.results.back();
    EXPECT_EQ(taker.status, SubmitStatus::FullyFilled);
    EXPECT_EQ(taker.resting_quantity.qty, 0);

    // Resting ask should now have 70 left at the same price.
    EXPECT_EQ(eng.book(kInstr)->best_ask_quantity().qty, 70);
    EXPECT_EQ(eng.book(kInstr)->best_ask().ticks, 100);
}

// ---------------------------------------------------------------------------
// Multi-level sweep
// ---------------------------------------------------------------------------
TEST(MatchingEngine, MultiLevelSweepFillsAcrossPriceLevels) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Asks: 30 @ 100, 40 @ 101, 50 @ 102.
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(30), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(101), q(40), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(102), q(50), TimeInForce::GTC, sink);

    // Incoming buy 120 @ 102: sweeps 30 + 40 + 50 = 120 exactly.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(102), q(120), TimeInForce::GTC, sink);

    ASSERT_EQ(sink.trades.size(), 3u);
    EXPECT_EQ(sink.trades[0].price.ticks, 100);
    EXPECT_EQ(sink.trades[0].quantity.qty, 30);
    EXPECT_EQ(sink.trades[1].price.ticks, 101);
    EXPECT_EQ(sink.trades[1].quantity.qty, 40);
    EXPECT_EQ(sink.trades[2].price.ticks, 102);
    EXPECT_EQ(sink.trades[2].quantity.qty, 50);

    EXPECT_EQ(sink.results.back().status, SubmitStatus::FullyFilled);
    EXPECT_TRUE(eng.book(kInstr)->best_ask().ticks == kInvalidPrice);
}

TEST(MatchingEngine, MultiLevelSweepPartialRestOnLastLevel) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(30), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(101), q(40), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(102), q(50), TimeInForce::GTC, sink);

    // Buy 100 @ 102: 30 + 40 + 30 (resting 20 on 102).
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(102), q(100), TimeInForce::GTC, sink);

    ASSERT_EQ(sink.trades.size(), 3u);
    EXPECT_EQ(sink.trades[0].quantity.qty, 30);
    EXPECT_EQ(sink.trades[1].quantity.qty, 40);
    EXPECT_EQ(sink.trades[2].quantity.qty, 30);

    EXPECT_EQ(sink.results.back().filled_quantity.qty, 100);
    EXPECT_EQ(sink.results.back().resting_quantity.qty, 0);
    EXPECT_EQ(eng.book(kInstr)->best_ask_quantity().qty, 20);
    EXPECT_EQ(eng.book(kInstr)->best_ask().ticks, 102);
}

// ---------------------------------------------------------------------------
// Limit order does NOT cross: must rest
// ---------------------------------------------------------------------------
TEST(MatchingEngine, LimitBuyDoesNotCrossRestsAtBack) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(50), TimeInForce::GTC, sink);

    // Buy 30 @ 99: ask is 100, 99 < 100, no cross, rests as bid.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(99), q(30), TimeInForce::GTC, sink);

    EXPECT_TRUE(sink.trades.empty());
    EXPECT_EQ(sink.results.back().status, SubmitStatus::Accepted);
    EXPECT_EQ(sink.results.back().resting_quantity.qty, 30);
    EXPECT_EQ(eng.book(kInstr)->best_bid().ticks, 99);
    EXPECT_EQ(eng.book(kInstr)->best_bid_quantity().qty, 30);
    EXPECT_EQ(eng.book(kInstr)->best_ask_quantity().qty, 50);
}

// ---------------------------------------------------------------------------
// Price priority: a higher bid is matched first
// ---------------------------------------------------------------------------
TEST(MatchingEngine, PricePriority_HigherBidHitsFirst) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Rest three asks at 200 so the bids below rest instead of matching.
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(200), q(10), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(200), q(10), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(200), q(10), TimeInForce::GTC, sink);

    // Three bids at 100. They rest (no ask at or below 100).
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(5),  TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(5),  TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(5),  TimeInForce::GTC, sink);

    // Capture the assigned IDs so the test doesn't depend on sequence
    // numbering internals (trades also consume sequence numbers).
    ASSERT_NE(eng.book(kInstr)->best_bid_order(), nullptr);
    OrderId id_a = eng.book(kInstr)->best_bid_order()->id;
    EXPECT_EQ(eng.book(kInstr)->total_order_count(), 6u);

    // Incoming sell 15 @ 100: must hit the three bids in FIFO order.
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(15), TimeInForce::GTC, sink);

    ASSERT_EQ(sink.trades.size(), 3u);
    EXPECT_EQ(sink.trades[0].buy_order_id, id_a);
    EXPECT_EQ(sink.trades[0].quantity.qty, 5);
    EXPECT_EQ(sink.trades[1].buy_order_id, id_a + 1);
    EXPECT_EQ(sink.trades[1].quantity.qty, 5);
    EXPECT_EQ(sink.trades[2].buy_order_id, id_a + 2);
    EXPECT_EQ(sink.trades[2].quantity.qty, 5);

    // All bids consumed, asks untouched.
    EXPECT_EQ(eng.book(kInstr)->total_order_count(), 3u);
}

// ---------------------------------------------------------------------------
// Time priority: at same price, older order is hit first
// ---------------------------------------------------------------------------
TEST(MatchingEngine, TimePriority_FifoAcrossThreeBids) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Three buys at the same price. They rest (no liquidity on the ask side).
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);

    OrderId id_first  = eng.book(kInstr)->best_bid_order()->id;
    OrderId id_second = id_first + 1;
    OrderId id_third  = id_first + 2;

    // Sell 150 @ 100: hits first bid (100) then second bid (50).
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(150), TimeInForce::GTC, sink);

    ASSERT_EQ(sink.trades.size(), 2u);
    EXPECT_EQ(sink.trades[0].buy_order_id, id_first);
    EXPECT_EQ(sink.trades[0].quantity.qty, 100);
    EXPECT_EQ(sink.trades[1].buy_order_id, id_second);
    EXPECT_EQ(sink.trades[1].quantity.qty, 50);

    // First bid is gone. Second bid has 50 remaining at the head of the
    // queue, followed by the untouched third bid (100).
    EXPECT_EQ(eng.book(kInstr)->total_order_count(), 2u);
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->id, id_second);
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->remaining.qty, 50);
    // Walk the level to confirm third bid still has 100.
    auto* nxt = eng.book(kInstr)->best_bid_order()->list_node.next;
    Order sample{};
    std::ptrdiff_t off = reinterpret_cast<char*>(&sample.list_node) -
                         reinterpret_cast<char*>(&sample);
    auto* third = reinterpret_cast<Order*>(reinterpret_cast<char*>(nxt) - off);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(third->id, id_third);
    EXPECT_EQ(third->remaining.qty, 100);
}

// ---------------------------------------------------------------------------
// Market order sweep
// ---------------------------------------------------------------------------
TEST(MatchingEngine, MarketBuySweepsMultipleLevels) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(10), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(101), q(20), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(102), q(30), TimeInForce::GTC, sink);

    // Market buy 55.
    auto o = std::make_unique<Order>();
    o->id = 100;  // pre-assigned
    o->trader_id = kTrader;
    o->instrument_id = kInstr;
    o->side = Side::Buy;
    o->type = OrderType::Market;
    o->price = Price{0};
    o->quantity = q(55);
    o->remaining = q(55);
    eng.submit(std::move(o), sink);

    ASSERT_EQ(sink.trades.size(), 3u);
    EXPECT_EQ(sink.trades[0].price.ticks, 100);
    EXPECT_EQ(sink.trades[1].price.ticks, 101);
    EXPECT_EQ(sink.trades[2].price.ticks, 102);
    EXPECT_EQ(sink.trades[0].quantity.qty, 10);
    EXPECT_EQ(sink.trades[1].quantity.qty, 20);
    EXPECT_EQ(sink.trades[2].quantity.qty, 25);

    EXPECT_EQ(sink.results.back().status, SubmitStatus::FullyFilled);
}

TEST(MatchingEngine, MarketOrderRejectedWithNoLiquidity) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    auto o = std::make_unique<Order>();
    o->id = 1;
    o->trader_id = kTrader;
    o->instrument_id = kInstr;
    o->side = Side::Buy;
    o->type = OrderType::Market;
    o->price = Price{0};
    o->quantity = q(100);
    o->remaining = q(100);
    eng.submit(std::move(o), sink);

    EXPECT_TRUE(sink.trades.empty());
    EXPECT_EQ(sink.results.back().status, SubmitStatus::Rejected);
    EXPECT_FALSE(sink.results.back().reject_reason.empty());
}

TEST(MatchingEngine, MarketOrderPartialFillThenReject) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(10), TimeInForce::GTC, sink);

    auto o = std::make_unique<Order>();
    o->id = 1;
    o->trader_id = kTrader;
    o->instrument_id = kInstr;
    o->side = Side::Buy;
    o->type = OrderType::Market;
    o->price = Price{0};
    o->quantity = q(50);
    o->remaining = q(50);
    eng.submit(std::move(o), sink);

    EXPECT_EQ(sink.trades.size(), 1u);
    EXPECT_EQ(sink.trades[0].quantity.qty, 10);
    EXPECT_EQ(sink.results.back().status, SubmitStatus::PartiallyFilled);
    EXPECT_EQ(sink.results.back().filled_quantity.qty, 10);
}

// ---------------------------------------------------------------------------
// IOC: unfilled portion is dropped
// ---------------------------------------------------------------------------
TEST(MatchingEngine, IocPartialFillDropsRemainder) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(10), TimeInForce::GTC, sink);

    // Buy 30 @ 100 IOC.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(30), TimeInForce::IOC, sink);

    ASSERT_EQ(sink.trades.size(), 1u);
    EXPECT_EQ(sink.trades[0].quantity.qty, 10);
    EXPECT_EQ(sink.results.back().status, SubmitStatus::PartiallyFilled);
    EXPECT_EQ(sink.results.back().filled_quantity.qty, 10);

    // Book is empty.
    EXPECT_TRUE(eng.book(kInstr)->best_ask().ticks == kInvalidPrice);
}

TEST(MatchingEngine, IocNoFillCancelsImmediately) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Sell 10 @ 100.
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(10), TimeInForce::GTC, sink);

    // Buy 30 @ 99 IOC: doesn't cross, remainder dropped.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(99), q(30), TimeInForce::IOC, sink);

    EXPECT_TRUE(sink.trades.empty());
    EXPECT_EQ(sink.results.back().status, SubmitStatus::Cancelled);
    EXPECT_EQ(eng.book(kInstr)->total_order_count(), 1u);  // only the original ask
}

// ---------------------------------------------------------------------------
// Validation rejects
// ---------------------------------------------------------------------------
TEST(MatchingEngine, RejectsUnregisteredInstrument) {
    MatchingEngine eng;
    CollectingSink sink;
    eng.submit_limit(/*instrument=*/9999, kTrader, Side::Buy, p(100), q(10),
                     TimeInForce::GTC, sink);
    EXPECT_EQ(sink.results.back().status, SubmitStatus::Rejected);
    EXPECT_EQ(sink.results.back().reject_reason, "instrument not registered");
}

TEST(MatchingEngine, RejectsInvalidQuantity) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    auto o = std::make_unique<Order>();
    o->id = 1;
    o->trader_id = kTrader;
    o->instrument_id = kInstr;
    o->side = Side::Buy;
    o->type = OrderType::Limit;
    o->price = p(100);
    o->quantity = Quantity{0};
    o->remaining = Quantity{0};
    eng.submit(std::move(o), sink);
    EXPECT_EQ(sink.results.back().status, SubmitStatus::Rejected);
    EXPECT_EQ(sink.results.back().reject_reason, "invalid quantity");
}

// ---------------------------------------------------------------------------
// Cancel works
// ---------------------------------------------------------------------------
TEST(MatchingEngine, CancelRestingOrder) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(50), TimeInForce::GTC, sink);
    EXPECT_TRUE(eng.cancel(kInstr, /*id=*/1));
    EXPECT_EQ(eng.book(kInstr)->total_order_count(), 0u);
}

TEST(MatchingEngine, CancelUnknownOrderReturnsFalse) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    EXPECT_FALSE(eng.cancel(kInstr, /*id=*/999));
}

// ---------------------------------------------------------------------------
// Sequence numbers are monotonic
// ---------------------------------------------------------------------------
TEST(MatchingEngine, SequenceNumbersAreMonotonic) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(10), TimeInForce::GTC, sink);
    Sequence s1 = sink.results.back().sequence;
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(10), TimeInForce::GTC, sink);
    Sequence s2 = sink.results.back().sequence;
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(10), TimeInForce::GTC, sink);
    Sequence s3 = sink.results.back().sequence;

    EXPECT_LT(s1, s2);
    EXPECT_LT(s2, s3);
    EXPECT_EQ(s1 + 1, s2);
    EXPECT_EQ(s2 + 1, s3);
}

// ---------------------------------------------------------------------------
// Multiple instruments are independent
// ---------------------------------------------------------------------------
TEST(MatchingEngine, MultipleInstrumentsAreIndependent) {
    MatchingEngine eng;
    eng.register_instrument(/*AAPL=*/1);
    eng.register_instrument(/*MSFT=*/2);

    CollectingSink sink;
    eng.submit_limit(1, kTrader, Side::Buy, p(100), q(10), TimeInForce::GTC, sink);
    eng.submit_limit(2, kTrader, Side::Buy, p(200), q(20), TimeInForce::GTC, sink);

    EXPECT_EQ(eng.book(1)->best_bid_quantity().qty, 10);
    EXPECT_EQ(eng.book(2)->best_bid_quantity().qty, 20);

    // A trade on instrument 1 should not affect instrument 2.
    eng.submit_limit(1, kTrader, Side::Sell, p(100), q(5), TimeInForce::GTC, sink);
    EXPECT_EQ(eng.book(2)->best_bid_quantity().qty, 20);
}

// ---------------------------------------------------------------------------
// Crossed book invariant: trade price = passive price
// ---------------------------------------------------------------------------
TEST(MatchingEngine, TradePriceIsPassivePrice) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(10), TimeInForce::GTC, sink);
    // Buy at higher price: still fills at 100.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(105), q(10), TimeInForce::GTC, sink);

    EXPECT_EQ(sink.trades[0].price.ticks, 100);
}