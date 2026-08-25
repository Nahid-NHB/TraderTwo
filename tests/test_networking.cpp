// tests/test_networking.cpp
//
// Tests for the line-oriented protocol parser and the in-memory
// Gateway::process_buffer path. We deliberately do not open real
// sockets in unit tests; production networking is exercised in the
// smoke test target.

#include "tt/common/types.hpp"
#include "tt/market_data/publisher.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/networking/gateway.hpp"
#include "tt/networking/protocol.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace tt;

namespace {

// Convenience: feed `commands` through process_buffer and return the
// concatenated response string.
std::string feed(const std::string& commands, MatchingEngine& eng,
                 MarketDataPublisher& pub) {
    return Gateway::process_buffer(commands, eng, pub);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Extract the order_id from an "OK ACCEPTED <id> <seq>" reply.
std::string extract_order_id(const std::string& reply) {
    auto pos = reply.find("OK ACCEPTED ");
    if (pos == std::string::npos) return {};
    auto oid_start = pos + std::string("OK ACCEPTED ").size();
    auto oid_end   = reply.find(' ', oid_start);
    if (oid_end == std::string::npos) return {};
    return reply.substr(oid_start, oid_end - oid_start);
}

}  // namespace

// ---------------------------------------------------------------------------
// Protocol parser
// ---------------------------------------------------------------------------
TEST(Protocol, Ping) {
    ParsedRequest out;
    auto err = parse_request("PING", out);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(out.command, InboundCommand::Ping);
}

TEST(Protocol, Quote) {
    ParsedRequest out;
    auto err = parse_request("QUOTE 7", out);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(out.command, InboundCommand::Quote);
    EXPECT_EQ(out.instrument, InstrumentId{7});
}

TEST(Protocol, NewLimit) {
    ParsedRequest out;
    auto err = parse_request("NEW 1 42 0 100 50 GTC", out);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(out.command, InboundCommand::New);
    EXPECT_EQ(out.instrument, InstrumentId{1});
    EXPECT_EQ(out.trader_id,  TraderId{42});
    EXPECT_EQ(out.side,       Side::Buy);
    EXPECT_EQ(out.type,       OrderType::Limit);
    EXPECT_EQ(out.tif,        TimeInForce::GTC);
    EXPECT_EQ(out.price.ticks, int64_t{100});
    EXPECT_EQ(out.qty.qty,    int64_t{50});
}

TEST(Protocol, NewSellSide1) {
    ParsedRequest out;
    auto err = parse_request("NEW 2 7 1 99 10 IOC", out);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(out.side, Side::Sell);
    EXPECT_EQ(out.tif,  TimeInForce::IOC);
}

TEST(Protocol, NewDefaultsToGTC) {
    ParsedRequest out;
    auto err = parse_request("NEW 1 1 0 100 10", out);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(out.tif, TimeInForce::GTC);
}

TEST(Protocol, Cancel) {
    ParsedRequest out;
    auto err = parse_request("CANCEL 1 42", out);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(out.command,   InboundCommand::Cancel);
    EXPECT_EQ(out.instrument, InstrumentId{1});
    EXPECT_EQ(out.order_id,  OrderId{42});
}

TEST(Protocol, Modify) {
    ParsedRequest out;
    auto err = parse_request("MODIFY 1 7 50 -12", out);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(out.command, InboundCommand::Modify);
    EXPECT_EQ(out.qty.qty, int64_t{50});
    EXPECT_EQ(out.price.ticks, int64_t{-12});
}

TEST(Protocol, BadCommand) {
    ParsedRequest out;
    auto err = parse_request("FOOBAR 1 2", out);
    EXPECT_FALSE(err.ok);
    EXPECT_FALSE(err.message.empty());
}

TEST(Protocol, EmptyLine) {
    ParsedRequest out;
    auto err = parse_request("", out);
    EXPECT_FALSE(err.ok);
}

TEST(Protocol, NewMissingArgs) {
    ParsedRequest out;
    auto err = parse_request("NEW 1 2", out);
    EXPECT_FALSE(err.ok);
}

TEST(Protocol, NewBadNumber) {
    ParsedRequest out;
    auto err = parse_request("NEW 1 2 0 abc 10", out);
    EXPECT_FALSE(err.ok);
}

// ---------------------------------------------------------------------------
// process_buffer end-to-end (no sockets)
// ---------------------------------------------------------------------------
TEST(ProcessBuffer, PingPong) {
    MatchingEngine eng;
    MarketDataPublisher pub(eng);
    auto resp = feed("PING\n", eng, pub);
    EXPECT_TRUE(contains(resp, "PONG"));
}

TEST(ProcessBuffer, QuoteUnknownInstrumentIsError) {
    MatchingEngine eng;
    MarketDataPublisher pub(eng);
    auto resp = feed("QUOTE 42\n", eng, pub);
    EXPECT_TRUE(contains(resp, "ERR"));
}

TEST(ProcessBuffer, SubmitAndQuote) {
    MatchingEngine eng;
    eng.register_instrument(InstrumentId{1});
    MarketDataPublisher pub(eng);

    std::string in;
    in += "NEW 1 100 1 100 10\n";  // sell 10 @ 100
    in += "NEW 1 101 0 99 5\n";    // buy 5 @ 99 (rests, no match)
    auto resp = feed(in, eng, pub);

    EXPECT_FALSE(contains(resp, "ERR"));

    auto resp2 = feed("QUOTE 1\n", eng, pub);
    EXPECT_TRUE(contains(resp2, "QUOTE 1"));
    EXPECT_TRUE(contains(resp2, " 99 "));
    EXPECT_TRUE(contains(resp2, " 100 "));
}

TEST(ProcessBuffer, CrossingTrades) {
    MatchingEngine eng;
    eng.register_instrument(InstrumentId{1});
    MarketDataPublisher pub(eng);

    std::string in;
    in += "NEW 1 1 1 100 10\n";  // sell 10 @ 100
    in += "NEW 1 2 0 100 5\n";   // buy  5 @ 100 (crosses, fills 5)
    auto resp = feed(in, eng, pub);

    EXPECT_TRUE(contains(resp, "TRADE"));
    EXPECT_TRUE(contains(resp, "OK FILLED"));
}

TEST(ProcessBuffer, Cancel) {
    MatchingEngine eng;
    eng.register_instrument(InstrumentId{1});
    MarketDataPublisher pub(eng);

    // Rest an order first, then cancel.
    auto r1 = feed("NEW 1 1 1 100 10 GTC\n", eng, pub);
    EXPECT_TRUE(contains(r1, "OK ACCEPTED"));

    auto oid = extract_order_id(r1);
    ASSERT_FALSE(oid.empty());

    auto r2 = feed("CANCEL 1 " + oid + "\n", eng, pub);
    EXPECT_FALSE(contains(r2, "ERR"));
}

TEST(ProcessBuffer, ModifyReducesQuantity) {
    MatchingEngine eng;
    eng.register_instrument(InstrumentId{1});
    MarketDataPublisher pub(eng);

    auto r1 = feed("NEW 1 1 1 100 10 GTC\n", eng, pub);
    auto oid = extract_order_id(r1);
    ASSERT_FALSE(oid.empty());

    auto r2 = feed("MODIFY 1 " + oid + " 5 100\n", eng, pub);
    EXPECT_FALSE(contains(r2, "ERR"));
}

TEST(ProcessBuffer, MultipleLinesAtOnce) {
    MatchingEngine eng;
    eng.register_instrument(InstrumentId{1});
    MarketDataPublisher pub(eng);

    std::string in;
    in += "PING\n";
    in += "QUOTE 1\n";        // empty book
    in += "NEW 1 1 1 100 5\n";
    in += "NEW 1 2 0 100 5\n";
    in += "QUOTE 1\n";
    in += "PING\n";

    auto resp = feed(in, eng, pub);

    // Two PONG replies.
    auto p1 = resp.find("PONG");
    auto p2 = resp.rfind("PONG");
    EXPECT_NE(p1, std::string::npos);
    EXPECT_NE(p2, p1);
    // At least one TRADE in the middle.
    EXPECT_TRUE(contains(resp, "TRADE"));
}

TEST(ProcessBuffer, ErrorDoesNotStopLaterCommands) {
    MatchingEngine eng;
    eng.register_instrument(InstrumentId{1});
    MarketDataPublisher pub(eng);

    std::string in;
    in += "FOOBAR\n";
    in += "PING\n";

    auto resp = feed(in, eng, pub);
    EXPECT_TRUE(contains(resp, "ERR"));
    EXPECT_TRUE(contains(resp, "PONG"));
}

TEST(ProcessBuffer, MixedOkAndError) {
    MatchingEngine eng;
    eng.register_instrument(InstrumentId{1});
    MarketDataPublisher pub(eng);

    std::string in;
    in += "NEW 1 1 1 100 10 GTC\n";
    in += "QUOTE 99\n";   // unknown instrument
    in += "PING\n";

    auto resp = feed(in, eng, pub);
    EXPECT_TRUE(contains(resp, "OK ACCEPTED"));
    EXPECT_TRUE(contains(resp, "ERR"));
    EXPECT_TRUE(contains(resp, "PONG"));
}