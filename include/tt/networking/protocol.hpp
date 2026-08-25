// include/tt/networking/protocol.hpp
//
// Simple line-oriented protocol on top of TCP. The gateway listens on a
// port and accepts one or more clients. Each client sends newline-delimited
// messages of the form:
//
//   NEW <instrument_id> <trader_id> <side> <price> <qty> [GTC|IOC|FOK]
//   CANCEL <instrument_id> <order_id>
//   MODIFY <instrument_id> <order_id> <new_qty> <new_price>
//   QUOTE <instrument_id>
//   PING
//
// Replies are also line-oriented. Example:
//   OK <submit_status> <order_id> <sequence>
//   TRADE <buy_id> <sell_id> <price> <qty>
//   QUOTE <bid> <bid_qty> <ask> <ask_qty>
//   ERR <message>
//   PONG

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tt {

enum class InboundCommand : uint8_t {
    Unknown = 0,
    New,
    Cancel,
    Modify,
    Quote,
    Ping,
};

struct ParsedRequest {
    InboundCommand command{InboundCommand::Unknown};
    InstrumentId   instrument{kInvalidInstrumentId};
    OrderId        order_id{kInvalidOrderId};
    TraderId       trader_id{0};
    Side           side{Side::Buy};
    OrderType      type{OrderType::Limit};
    TimeInForce    tif{TimeInForce::GTC};
    Price          price{Price{0}};
    Quantity       qty{Quantity{0}};
};

struct ParseError {
    bool ok{true};
    std::string message;
};

// Parse a single line. On failure, returns an error.
ParseError parse_request(std::string_view line, ParsedRequest& out);

// Serialise submit/accept reply.
std::string format_submit_reply(OrderId id, Sequence seq, const std::string& status);

// Serialise trade reply.
std::string format_trade(const Trade& t);

// Serialise top-of-book reply.
std::string format_quote(InstrumentId id, Price bid, Quantity bid_qty,
                         Price ask, Quantity ask_qty);

// Serialise error reply.
std::string format_error(const std::string& msg);

}  // namespace tt