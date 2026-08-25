// tests/test_event_log.cpp
//
// Tests for the event log + replay.

#include "tt/common/types.hpp"
#include "tt/market_data/events.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/persistence/event_log.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

using namespace tt;

namespace {
Price p(int64_t v) { return Price{v}; }
Quantity q(int64_t v) { return Quantity{v}; }

std::string temp_path() {
    static int counter = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/tmp/tt_event_log_%d_%d.bin",
                  static_cast<int>(getpid()), counter++);
    return std::string(buf);
}

// Read records without opening a writer.
std::vector<PersistedEvent> read_records(const std::string& path) {
    EventLog reader(path, /*append=*/true);
    return reader.read_all();
}
}  // namespace

TEST(EventLog, WritesAndReadsTradeRecords) {
    auto path = temp_path();
    {
        EventLog log(path);
        PersistingSink sink(log);
        sink.on_trade(Trade{
            .id            = 1,
            .instrument_id = 42,
            .buy_order_id  = 100,
            .sell_order_id = 101,
            .buy_trader_id = 1,
            .sell_trader_id= 2,
            .price         = p(12345),
            .quantity      = q(10),
            .sequence      = 5,
            .timestamp     = 999,
        });
    }
    auto records = read_records(path);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].kind, EventKind::Trade);
    EXPECT_EQ(records[0].trade.id, 1u);
    EXPECT_EQ(records[0].trade.price.ticks, 12345);
}

TEST(EventLog, RecordsOrderAccepted) {
    auto path = temp_path();
    {
        EventLog log(path);
        PersistingSink sink(log);
        SubmitResult r{};
        r.status        = SubmitStatus::Accepted;
        r.order_id      = 7;
        r.instrument_id = 42;
        r.sequence      = 11;
        r.resting_quantity = q(50);
        sink.on_submit_result(r);
    }
    auto records = read_records(path);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].kind, EventKind::OrderRested);
    EXPECT_EQ(records[0].order.order_id, 7u);
}

TEST(EventLog, RecordsRejectReason) {
    auto path = temp_path();
    {
        EventLog log(path);
        PersistingSink sink(log);
        SubmitResult r{};
        r.status        = SubmitStatus::Rejected;
        r.order_id      = 7;
        r.instrument_id = 42;
        r.sequence      = 11;
        r.reject_reason = "invalid quantity";
        sink.on_submit_result(r);
    }
    auto records = read_records(path);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].kind, EventKind::OrderRejected);
    EXPECT_EQ(records[0].reject_reason, "invalid quantity");
}

TEST(EventLog, MultipleRecordsWrittenAndRead) {
    auto path = temp_path();
    {
        EventLog log(path);
        PersistingSink sink(log);
        // Two order submissions and a trade.
        SubmitResult r1{};
        r1.status = SubmitStatus::Accepted;
        r1.order_id = 1;
        r1.instrument_id = 42;
        r1.sequence = 1;
        r1.resting_quantity = q(10);
        sink.on_submit_result(r1);

        SubmitResult r2{};
        r2.status = SubmitStatus::Accepted;
        r2.order_id = 2;
        r2.instrument_id = 42;
        r2.sequence = 2;
        r2.resting_quantity = q(20);
        sink.on_submit_result(r2);

        sink.on_trade(Trade{
            .id = 100, .instrument_id = 42,
            .buy_order_id = 1, .sell_order_id = 2,
            .buy_trader_id = 1, .sell_trader_id = 2,
            .price = p(123), .quantity = q(5),
            .sequence = 3, .timestamp = 100,
        });
    }
    auto records = read_records(path);
    EXPECT_EQ(records.size(), 3u);
}

TEST(Replay, RoundTripRestoresBook) {
    auto path = temp_path();

    MatchingEngine eng;
    eng.register_instrument(42);
    {
        EventLog log(path);
        CollectingSink cs;
        PersistingSink ps(log);
        class TeeSink final : public TradeSink {
        public:
            TeeSink(TradeSink& a, TradeSink& b) : a_(a), b_(b) {}
            void on_trade(const Trade& t) noexcept override {
                a_.on_trade(t); b_.on_trade(t);
            }
            void on_submit_result(const SubmitResult& r) noexcept override {
                a_.on_submit_result(r); b_.on_submit_result(r);
            }
        private:
            TradeSink& a_; TradeSink& b_;
        };
        TeeSink tee(cs, ps);
        eng.submit_limit(42, 1, Side::Sell, p(100), q(50), TimeInForce::GTC, tee);
        eng.submit_limit(42, 2, Side::Sell, p(101), q(20), TimeInForce::GTC, tee);
        eng.submit_limit(42, 3, Side::Buy,  p(100), q(30), TimeInForce::GTC, tee);
    }

    MatchingEngine eng2;
    eng2.register_instrument(42);

    auto records = read_records(path);
    Replayer rep(eng2);
    auto applied = rep.replay(records);
    EXPECT_GT(applied, 0u);

    EXPECT_EQ(eng2.book(42)->best_ask().ticks, 100);
    EXPECT_EQ(eng2.book(42)->best_ask_quantity().qty, 20);
}