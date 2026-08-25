// tests/test_risk.cpp
//
// Tests for the pre-trade risk layer.

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/risk/risk.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace tt;

namespace {
Price p(int64_t v) { return Price{v}; }
Quantity q(int64_t v) { return Quantity{v}; }

std::unique_ptr<Order> make_order(TraderId trader, Side side,
                                  Price price, Quantity qty) {
    auto o = std::make_unique<Order>();
    o->trader_id     = trader;
    o->instrument_id = 42;
    o->side          = side;
    o->type          = OrderType::Limit;
    o->tif           = TimeInForce::GTC;
    o->price         = price;
    o->quantity      = qty;
    o->remaining     = qty;
    o->status        = OrderStatus::New;
    return o;
}
}  // namespace

// ---------------------------------------------------------------------------
// MaxOrderQuantityCheck
// ---------------------------------------------------------------------------
TEST(Risk, MaxOrderQuantityBlocks) {
    MaxOrderQuantityCheck c(q(100));
    Order o{};
    o.quantity = q(101);
    auto d = c.evaluate(o);
    EXPECT_FALSE(d.ok());
    EXPECT_EQ(d.code, RiskRejectCode::MaxQuantity);
}

TEST(Risk, MaxOrderQuantityAllowsBelowCap) {
    MaxOrderQuantityCheck c(q(100));
    Order o{};
    o.quantity = q(100);
    EXPECT_TRUE(c.evaluate(o).ok());
    o.quantity = q(50);
    EXPECT_TRUE(c.evaluate(o).ok());
}

// ---------------------------------------------------------------------------
// MaxNotionalCheck
// ---------------------------------------------------------------------------
TEST(Risk, MaxNotionalBlocksLargeOrder) {
    MaxNotionalCheck c(/*max_ticks=*/10000);
    Order o{};
    o.quantity = q(100);
    o.price    = p(200);   // 200*100 = 20000 > 10000
    auto d = c.evaluate(o);
    EXPECT_FALSE(d.ok());
    EXPECT_EQ(d.code, RiskRejectCode::MaxNotional);
}

TEST(Risk, MaxNotionalAllowsSmallOrder) {
    MaxNotionalCheck c(/*max_ticks=*/100000);
    Order o{};
    o.quantity = q(100);
    o.price    = p(50);   // 5000 < 100000
    EXPECT_TRUE(c.evaluate(o).ok());
}

// ---------------------------------------------------------------------------
// PriceCollarCheck
// ---------------------------------------------------------------------------
TEST(Risk, PriceCollarRejectsOutOfBand) {
    PriceCollarCheck c(p(50), p(150));
    Order o{};
    o.price = p(40);
    EXPECT_FALSE(c.evaluate(o).ok());
    o.price = p(160);
    EXPECT_FALSE(c.evaluate(o).ok());
}

TEST(Risk, PriceCollarAcceptsInBand) {
    PriceCollarCheck c(p(50), p(150));
    Order o{};
    o.price = p(100);
    EXPECT_TRUE(c.evaluate(o).ok());
    o.price = p(50);
    EXPECT_TRUE(c.evaluate(o).ok());
    o.price = p(150);
    EXPECT_TRUE(c.evaluate(o).ok());
}

TEST(Risk, PriceCollarIgnoresZeroBounds) {
    PriceCollarCheck c(p(0), p(0));
    Order o{};
    o.price = p(1'000'000);
    EXPECT_TRUE(c.evaluate(o).ok());
}

// ---------------------------------------------------------------------------
// RateLimitCheck
// ---------------------------------------------------------------------------
TEST(Risk, RateLimitAllowsBurst) {
    RateLimitCheck c(/*burst=*/5, /*refill_per_second=*/1.0);
    Order o{};
    o.trader_id = 1;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(c.evaluate(o).ok()) << "burst index " << i;
    }
    EXPECT_FALSE(c.evaluate(o).ok());
}

TEST(Risk, RateLimitPerTrader) {
    RateLimitCheck c(/*burst=*/1, /*refill_per_second=*/1000.0);
    Order o{};
    o.trader_id = 1;
    o.trader_id = 1;
    EXPECT_TRUE(c.evaluate(o).ok());
    EXPECT_FALSE(c.evaluate(o).ok());

    o.trader_id = 2;
    EXPECT_TRUE(c.evaluate(o).ok());
    EXPECT_FALSE(c.evaluate(o).ok());
}

// ---------------------------------------------------------------------------
// RiskGate composition
// ---------------------------------------------------------------------------
TEST(RiskGate, NoChecksAlwaysAllows) {
    RiskGate g;
    Order o{};
    EXPECT_TRUE(g.evaluate(o).ok());
}

TEST(RiskGate, FirstFailureShortCircuits) {
    RiskGate g;
    g.add_check(std::make_unique<MaxOrderQuantityCheck>(q(10)));
    g.add_check(std::make_unique<AlwaysAllow>());
    Order o{};
    o.quantity = q(20);
    auto d = g.evaluate(o);
    EXPECT_FALSE(d.ok());
    EXPECT_EQ(d.code, RiskRejectCode::MaxQuantity);
}

TEST(RiskGate, AllPassThenAllowed) {
    RiskGate g;
    g.add_check(std::make_unique<MaxOrderQuantityCheck>(q(100)));
    g.add_check(std::make_unique<PriceCollarCheck>(p(50), p(150)));
    Order o{};
    o.quantity = q(50);
    o.price    = p(100);
    EXPECT_TRUE(g.evaluate(o).ok());
}

// ---------------------------------------------------------------------------
// Engine integration
// ---------------------------------------------------------------------------
TEST(RiskEngine, RiskGateBlocksSubmission) {
    MatchingEngine eng;
    eng.register_instrument(42);
    auto gate = std::make_shared<RiskGate>();
    gate->add_check(std::make_unique<MaxOrderQuantityCheck>(q(10)));
    eng.set_risk_gate(gate);

    CollectingSink sink;
    eng.submit_limit(42, 1, Side::Sell, p(100), q(20), TimeInForce::GTC, sink);
    EXPECT_EQ(sink.results.back().status, SubmitStatus::Rejected);
    EXPECT_NE(sink.results.back().reject_reason.find("risk"), std::string::npos);

    // Below cap should pass.
    eng.submit_limit(42, 1, Side::Sell, p(100), q(5), TimeInForce::GTC, sink);
    EXPECT_EQ(sink.results.back().status, SubmitStatus::Accepted);
}