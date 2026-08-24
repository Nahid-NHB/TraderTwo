// tests/test_order_book.cpp
//
// Tests for the price-time priority order book.

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/orderbook/order_book.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace tt;

namespace {

// Helper: build a resting limit order ready for insertion.
std::unique_ptr<Order> make_limit(OrderId id, Side side, Price price, Quantity qty,
                                  Sequence seq) {
    auto o = std::make_unique<Order>();
    o->id            = id;
    o->trader_id     = 1;
    o->instrument_id = 42;
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
// Basic insert + best quote
// ---------------------------------------------------------------------------
TEST(OrderBook, InsertSingleBidAndAsk) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy,  Price{100'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Sell, Price{101'000'000}, Quantity{150}, 2)));

    EXPECT_EQ(book.best_bid().ticks, 100'000'000);
    EXPECT_EQ(book.best_ask().ticks, 101'000'000);
    EXPECT_EQ(book.best_bid_quantity().qty, 100);
    EXPECT_EQ(book.best_ask_quantity().qty, 150);
    EXPECT_EQ(book.total_order_count(), 2u);
}

TEST(OrderBook, EmptyBookHasNoBestQuote) {
    OrderBook book(42);
    EXPECT_FALSE(book.best_bid().is_valid());
    EXPECT_FALSE(book.best_ask().is_valid());
    EXPECT_TRUE(book.empty());
}

// ---------------------------------------------------------------------------
// Price priority: best bid/ask wins regardless of arrival order.
// ---------------------------------------------------------------------------
TEST(OrderBook, PricePriority_BidsSortedDescending) {
    OrderBook book(42);
    // Insert out of order: lowest first, then higher.
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy, Price{ 99'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Buy, Price{101'000'000}, Quantity{100}, 2)));
    ASSERT_TRUE(book.insert(make_limit(3, Side::Buy, Price{100'000'000}, Quantity{100}, 3)));

    EXPECT_EQ(book.best_bid().ticks, 101'000'000);
    EXPECT_EQ(book.bid_level_count(), 3u);
}

TEST(OrderBook, PricePriority_AsksSortedAscending) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Sell, Price{102'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Sell, Price{100'000'000}, Quantity{100}, 2)));
    ASSERT_TRUE(book.insert(make_limit(3, Side::Sell, Price{101'000'000}, Quantity{100}, 3)));

    EXPECT_EQ(book.best_ask().ticks, 100'000'000);
    EXPECT_EQ(book.ask_level_count(), 3u);
}

// ---------------------------------------------------------------------------
// Time priority: at the same price, the older order is at the head of the
// queue and is the first to be matched.
// ---------------------------------------------------------------------------
TEST(OrderBook, TimePriority_FifoAtSameLevel) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy, Price{100'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Buy, Price{100'000'000}, Quantity{200}, 2)));
    ASSERT_TRUE(book.insert(make_limit(3, Side::Buy, Price{100'000'000}, Quantity{300}, 3)));

    Order* head = book.best_bid_order();
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head->id, 1u);
    EXPECT_EQ(head->remaining.qty, 100);
}

TEST(OrderBook, CancelPreservesOtherOrdersFifoOrder) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy, Price{100'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Buy, Price{100'000'000}, Quantity{200}, 2)));
    ASSERT_TRUE(book.insert(make_limit(3, Side::Buy, Price{100'000'000}, Quantity{300}, 3)));

    ASSERT_TRUE(book.cancel(2));
    EXPECT_EQ(book.best_bid_order()->id, 1u);
    EXPECT_EQ(book.total_order_count(), 2u);
    EXPECT_EQ(book.best_bid_quantity().qty, 400);  // 100 + 300

    // After cancelling the head, id=3 should become the new head.
    ASSERT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_bid_order()->id, 3u);
}

TEST(OrderBook, CancelRemovesEmptyLevel) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy, Price{100'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Buy, Price{101'000'000}, Quantity{100}, 2)));

    EXPECT_EQ(book.bid_level_count(), 2u);
    ASSERT_TRUE(book.cancel(1));
    EXPECT_EQ(book.bid_level_count(), 1u);
    EXPECT_EQ(book.best_bid().ticks, 101'000'000);
}

// ---------------------------------------------------------------------------
// Aggregated quantity at best price
// ---------------------------------------------------------------------------
TEST(OrderBook, AggregatedQuantityAtBestPrice) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy, Price{100'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Buy, Price{100'000'000}, Quantity{250}, 2)));
    ASSERT_TRUE(book.insert(make_limit(3, Side::Buy, Price{100'000'000}, Quantity{ 50}, 3)));
    ASSERT_TRUE(book.insert(make_limit(4, Side::Buy, Price{ 99'000'000}, Quantity{999}, 4)));

    EXPECT_EQ(book.best_bid_quantity().qty, 400);  // 100+250+50
    EXPECT_EQ(book.best_bid().ticks, 100'000'000);
}

// ---------------------------------------------------------------------------
// Cancel error paths
// ---------------------------------------------------------------------------
TEST(OrderBook, CancelUnknownOrderReturnsFalse) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy, Price{100'000'000}, Quantity{100}, 1)));
    EXPECT_FALSE(book.cancel(999));
}

TEST(OrderBook, DuplicateOrderIdIsRejected) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy, Price{100'000'000}, Quantity{100}, 1)));
    EXPECT_FALSE(book.insert(make_limit(1, Side::Sell, Price{101'000'000}, Quantity{100}, 2)));
    EXPECT_EQ(book.total_order_count(), 1u);
}

TEST(OrderBook, MarketOrdersAreRejectedInPhase1) {
    OrderBook book(42);
    auto o = std::make_unique<Order>();
    o->id            = 1;
    o->trader_id     = 1;
    o->instrument_id = 42;
    o->side          = Side::Buy;
    o->type          = OrderType::Market;
    o->price         = Price{0};
    o->quantity      = Quantity{100};
    o->remaining     = Quantity{100};
    o->sequence      = 1;
    EXPECT_FALSE(book.insert(std::move(o)));
}

// ---------------------------------------------------------------------------
// Depth snapshot
// ---------------------------------------------------------------------------
TEST(OrderBook, DepthSnapshotTopN) {
    OrderBook book(42);
    ASSERT_TRUE(book.insert(make_limit(1, Side::Buy,  Price{ 99'000'000}, Quantity{100}, 1)));
    ASSERT_TRUE(book.insert(make_limit(2, Side::Buy,  Price{100'000'000}, Quantity{200}, 2)));
    ASSERT_TRUE(book.insert(make_limit(3, Side::Buy,  Price{101'000'000}, Quantity{300}, 3)));
    ASSERT_TRUE(book.insert(make_limit(4, Side::Sell, Price{102'000'000}, Quantity{150}, 4)));
    ASSERT_TRUE(book.insert(make_limit(5, Side::Sell, Price{103'000'000}, Quantity{250}, 5)));

    std::vector<OrderBook::DepthLevel> bids, asks;
    book.depth(2, bids, asks);

    ASSERT_EQ(bids.size(), 2u);
    ASSERT_EQ(asks.size(), 2u);
    // Bids are descending.
    EXPECT_EQ(bids[0].price.ticks, 101'000'000);
    EXPECT_EQ(bids[0].total_quantity.qty, 300);
    EXPECT_EQ(bids[1].price.ticks, 100'000'000);
    EXPECT_EQ(bids[1].total_quantity.qty, 200);
    // Asks are ascending.
    EXPECT_EQ(asks[0].price.ticks, 102'000'000);
    EXPECT_EQ(asks[1].price.ticks, 103'000'000);
}