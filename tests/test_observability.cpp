// tests/test_observability.cpp
//
// Tests for the metrics + structured logging layer (Phase 12).

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/observability/metrics.hpp"
#include "tt/observability/structured_log.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

using namespace tt;

namespace {

// Helper to make a non-default SubmitResult for json_submit tests.
SubmitResult make_submit(SubmitStatus s, OrderId id, Sequence seq,
                         InstrumentId instr = InstrumentId{1},
                         const std::string& reason = "") {
    SubmitResult r{};
    r.status        = s;
    r.order_id      = id;
    r.sequence      = seq;
    r.instrument_id = instr;
    r.filled_quantity = Quantity{5};
    r.resting_quantity = Quantity{3};
    r.price = Price{100};
    r.reject_reason = reason;
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// CounterBundle snapshot
// ---------------------------------------------------------------------------
TEST(Metrics, SnapshotInitiallyZero) {
    CounterBundle b;
    auto s = b.snapshot();
    EXPECT_EQ(s.submits_accepted, 0u);
    EXPECT_EQ(s.trades_total, 0u);
    EXPECT_EQ(s.last_sequence, 0u);
}

TEST(Metrics, SinkIncrementsSubmitCounts) {
    CounterBundle b;
    MetricsSink sink(b);

    sink.on_submit_result(make_submit(SubmitStatus::Accepted,        OrderId{1}, Sequence{1}));
    sink.on_submit_result(make_submit(SubmitStatus::FullyFilled,     OrderId{2}, Sequence{2}));
    sink.on_submit_result(make_submit(SubmitStatus::PartiallyFilled, OrderId{3}, Sequence{3}));
    sink.on_submit_result(make_submit(SubmitStatus::Rejected,        OrderId{4}, Sequence{4}, InstrumentId{1}, "bad"));
    sink.on_submit_result(make_submit(SubmitStatus::Cancelled,       OrderId{5}, Sequence{5}));

    auto s = b.snapshot();
    EXPECT_EQ(s.submits_accepted,  1u);
    EXPECT_EQ(s.submits_filled,    1u);
    EXPECT_EQ(s.submits_partial,   1u);
    EXPECT_EQ(s.submits_rejected,  1u);
    EXPECT_EQ(s.submits_cancelled, 1u);
    EXPECT_EQ(s.last_sequence, 5u);
}

TEST(Metrics, SinkIncrementsTradeCounts) {
    CounterBundle b;
    MetricsSink sink(b);

    Trade t{};
    t.buy_order_id  = OrderId{1};
    t.sell_order_id = OrderId{2};
    t.price         = Price{100};
    t.quantity      = Quantity{10};
    t.sequence      = Sequence{42};

    sink.on_trade(t);
    sink.on_trade(t);
    sink.on_trade(t);

    auto s = b.snapshot();
    EXPECT_EQ(s.trades_total, 3u);
    EXPECT_EQ(s.trade_qty_total, 30u);
    EXPECT_EQ(s.trade_notional_total, 100u * 10u * 3u);
    EXPECT_EQ(s.last_sequence, 42u);
}

TEST(Metrics, SinkChainsDownstream) {
    CounterBundle b;
    CollectingSink downstream;
    MetricsSink sink(b, &downstream);

    sink.on_submit_result(make_submit(SubmitStatus::Accepted, OrderId{1}, Sequence{1}));

    EXPECT_EQ(downstream.results.size(), 1u);
    EXPECT_EQ(b.submits_accepted.load(), 1u);
}

TEST(Metrics, CancelModifyHooks) {
    CounterBundle b;
    MetricsSink sink(b);
    sink.record_cancel_attempt(true);
    sink.record_cancel_attempt(false);
    sink.record_modify_attempt(true);

    auto s = b.snapshot();
    EXPECT_EQ(s.cancels_attempted, 2u);
    EXPECT_EQ(s.cancels_succeeded, 1u);
    EXPECT_EQ(s.modifies_attempted, 1u);
    EXPECT_EQ(s.modifies_succeeded, 1u);
}

TEST(Metrics, RegistryPerInstrument) {
    MetricsRegistry reg;
    auto& s1 = reg.for_instrument(InstrumentId{1});
    auto& s2 = reg.for_instrument(InstrumentId{2});
    EXPECT_NE(&s1, &s2);
    s1.submits_accepted.store(7);
    s2.trades_total.store(11);
    EXPECT_EQ(reg.for_instrument(InstrumentId{1}).submits_accepted.load(), 7u);
    EXPECT_EQ(reg.for_instrument(InstrumentId{2}).trades_total.load(), 11u);
}

TEST(Metrics, RenderPrometheusHasExpectedKeys) {
    CounterBundle b;
    b.submits_accepted.store(3);
    auto s = b.snapshot();
    auto out = render_prometheus(s, "tt");
    EXPECT_NE(out.find("tt_submits_total"), std::string::npos);
    EXPECT_NE(out.find("tt_trades_total"), std::string::npos);
    EXPECT_NE(out.find("tt_last_sequence"), std::string::npos);
    EXPECT_NE(out.find("status=\"accepted\""), std::string::npos);
}

TEST(Metrics, RenderJsonRoundTrip) {
    CounterBundle b;
    b.submits_accepted.store(2);
    b.trades_total.store(5);
    auto s = b.snapshot();
    auto out = render_json(s);
    EXPECT_NE(out.find("\"submits\""), std::string::npos);
    EXPECT_NE(out.find("\"trades\""), std::string::npos);
    EXPECT_NE(out.find("\"accepted\":2"), std::string::npos);
    EXPECT_NE(out.find("\"total\":5"), std::string::npos);
}

TEST(Metrics, ResetClearsEverything) {
    CounterBundle b;
    b.submits_accepted.store(10);
    b.trades_total.store(20);
    b.reset();
    auto s = b.snapshot();
    EXPECT_EQ(s.submits_accepted, 0u);
    EXPECT_EQ(s.trades_total, 0u);
}

// ---------------------------------------------------------------------------
// Structured logger
// ---------------------------------------------------------------------------
TEST(StructuredLog, WritesJsonToFile) {
    const std::string path = "/tmp/tt_logger_test.log";
    std::remove(path.c_str());

    {
        StructuredLogger log(path);
        log.info("trade",  std::string(R"({"foo":"bar"})"));
        log.warn("submit", std::string(R"({"order_id":42})"));
        log.flush();
    }

    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    auto contents = ss.str();
    EXPECT_FALSE(contents.empty());
    EXPECT_NE(contents.find("\"event\":\"trade\""), std::string::npos);
    EXPECT_NE(contents.find("\"event\":\"submit\""), std::string::npos);
    EXPECT_NE(contents.find("\"level\":\"INFO\""), std::string::npos);
    EXPECT_NE(contents.find("\"level\":\"WARN\""), std::string::npos);
    EXPECT_NE(contents.find("\"foo\":\"bar\""), std::string::npos);
    EXPECT_NE(contents.find("\"order_id\":42"), std::string::npos);
    EXPECT_NE(contents.find("\"ts\":\""), std::string::npos);
}

TEST(StructuredLog, EscapesQuotesAndBackslashes) {
    const std::string path = "/tmp/tt_logger_escape.log";
    std::remove(path.c_str());

    // The `event` field IS escaped by the logger. Provide an event name with
    // quotes/backslashes and verify they end up escaped in the on-disk
    // output, while still producing valid JSON.
    std::string event = "weird\"event\\name";

    {
        StructuredLogger log(path);
        log.info(event, std::string(R"({"a":1})"));
        log.flush();
    }

    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    auto contents = ss.str();
    // The output should escape the quote in the event field. The escape
    // function writes the backslash and quote as \\ and \".
    EXPECT_NE(contents.find("weird\\\"event\\\\name"), std::string::npos)
        << "got: " << contents;
}

TEST(StructuredLog, JsonHelpersProduceParseableOutput) {
    SubmitResult r = make_submit(SubmitStatus::FullyFilled, OrderId{99}, Sequence{7});
    auto js = json_submit(r);
    EXPECT_NE(js.find("\"order_id\":99"), std::string::npos);
    EXPECT_NE(js.find("\"status\":\"filled\""), std::string::npos);

    Trade t{};
    t.buy_order_id  = OrderId{1};
    t.sell_order_id = OrderId{2};
    t.price         = Price{50};
    t.quantity      = Quantity{3};
    t.sequence      = Sequence{10};
    auto jt = json_trade(t);
    EXPECT_NE(jt.find("\"buy_order_id\":1"), std::string::npos);
    EXPECT_NE(jt.find("\"qty\":3"), std::string::npos);
}

TEST(StructuredLog, StripsTrailingBraceInKv) {
    // The logger should strip a single trailing '}' from the user-provided
    // kv string before adding its own. Without stripping we'd get }} at the
    // end of every line.
    const std::string path = "/tmp/tt_logger_trim.log";
    std::remove(path.c_str());

    {
        StructuredLogger log(path);
        log.info("x", std::string(R"({"a":1})"));
        log.flush();
    }

    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    auto contents = ss.str();
    // Should NOT end with "}}" — strip works.
    EXPECT_EQ(contents.find("}}"), std::string::npos);
    // Should contain the trimmed body before the closing brace.
    EXPECT_NE(contents.find(R"("a":1)"), std::string::npos);
}