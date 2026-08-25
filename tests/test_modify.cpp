// tests/test_modify.cpp
//
// Tests for cancel/modify: priority reset on price change, in-place
// reduction on quantity decrease, reject paths.

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

std::unique_ptr<Order> make_limit(OrderId id, Side side, Price price,
                                  Quantity qty, Sequence seq) {
    auto o = std::make_unique<Order>();
    o->id            = id;
    o->trader_id     = kTrader;
    o->instrument_id = kInstr;
    o->side          = side;
    o->type          = OrderType::Limit;
    o->tif           = TimeInForce::GTC;
    o->price         = price;
    o->quantity      = qty;
    o->remaining     = qty;
    o->sequence      = seq;
    o->status        = OrderStatus::New;
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// Reduce-only modify preserves time priority
// ---------------------------------------------------------------------------
TEST(Modify, ReduceQuantityKeepsPriority) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Three buys at the same price.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);

    OrderId id_first  = eng.book(kInstr)->best_bid_order()->id;
    OrderId id_second = id_first + 1;
    OrderId id_third  = id_first + 2;

    // Reduce the first order from 100 -> 50. It should remain the head.
    auto result = eng.modify(kInstr, id_first, q(50), p(100));
    EXPECT_EQ(result, OrderBook::ModifyResult::Modified);

    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->id, id_first);
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->remaining.qty, 50);

    // A subsequent sell should still hit id_first (FIFO preserved).
    eng.submit_limit(kInstr, kTrader, Side::Sell, p(100), q(40), TimeInForce::GTC, sink);
    ASSERT_EQ(sink.trades.size(), 1u);
    EXPECT_EQ(sink.trades[0].buy_order_id, id_first);
    EXPECT_EQ(sink.trades[0].quantity.qty, 40);
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->id, id_first);
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->remaining.qty, 10);
    (void)id_second;
    (void)id_third;
}

// ---------------------------------------------------------------------------
// Price change resets priority (replaces at back of new level)
// ---------------------------------------------------------------------------
TEST(Modify, PriceChangeResetsPriority) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    // Two buys at 100; the first will be modified.
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    OrderId id_first  = eng.book(kInstr)->best_bid_order()->id;
    OrderId id_second = id_first + 1;

    // Move the first to a different price -> priority reset.
    auto result = eng.modify(kInstr, id_first, q(100), p(99));
    EXPECT_EQ(result, OrderBook::ModifyResult::Replaced);

    // Best bid is now id_second at 100. The modified order rests at 99.
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->id, id_second);
    EXPECT_EQ(eng.book(kInstr)->best_bid().ticks, 100);

    // Now modify id_first (at 99) to 100 -> replaces at back.
    eng.modify(kInstr, id_first, q(100), p(100));
    // id_second should still be at the head.
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->id, id_second);
    // The book has both orders at 100 in FIFO order: id_second then id_first.
}

// ---------------------------------------------------------------------------
// Quantity increase resets priority
// ---------------------------------------------------------------------------
TEST(Modify, QuantityIncreaseResetsPriority) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    OrderId id_first  = eng.book(kInstr)->best_bid_order()->id;
    OrderId id_second = id_first + 1;

    auto result = eng.modify(kInstr, id_first, q(200), p(100));
    EXPECT_EQ(result, OrderBook::ModifyResult::Replaced);

    // id_second should still be at the head.
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->id, id_second);
}

// ---------------------------------------------------------------------------
// Modify on missing order returns NotFound
// ---------------------------------------------------------------------------
TEST(Modify, MissingOrderReturnsNotFound) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    auto result = eng.modify(kInstr, /*id=*/999, Quantity{100}, Price{100});
    EXPECT_EQ(result, OrderBook::ModifyResult::NotFound);
}

// ---------------------------------------------------------------------------
// Reduce helper
// ---------------------------------------------------------------------------
TEST(Modify, ReduceHelperWorks) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;

    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(100), TimeInForce::GTC, sink);
    OrderId id = eng.book(kInstr)->best_bid_order()->id;

    EXPECT_TRUE(eng.reduce(kInstr, id, q(50)));
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->remaining.qty, 50);
    // Still at the head (priority preserved).
    EXPECT_EQ(eng.book(kInstr)->best_bid_order()->id, id);

    // Reducing to zero or to >= remaining is rejected.
    EXPECT_FALSE(eng.reduce(kInstr, id, q(0)));
    EXPECT_FALSE(eng.reduce(kInstr, id, q(60)));
}

// ---------------------------------------------------------------------------
// Cancel works through the matching engine (already exercised in Phase 2,
// repeated here for completeness).
// ---------------------------------------------------------------------------
TEST(Modify, CancelRemovesOrder) {
    MatchingEngine eng;
    eng.register_instrument(kInstr);
    CollectingSink sink;
    eng.submit_limit(kInstr, kTrader, Side::Buy, p(100), q(50), TimeInForce::GTC, sink);
    OrderId id = eng.book(kInstr)->best_bid_order()->id;
    EXPECT_TRUE(eng.cancel(kInstr, id));
    EXPECT_EQ(eng.book(kInstr)->total_order_count(), 0u);
}
