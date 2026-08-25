// include/tt/market_data/events.hpp
//
// Domain events emitted by the matching engine. Designed to be trivial to
// forward to a logger, a multicast publisher, or an event log.
//
// The MatchingEngine emits events through the TradeSink callback. A separate
// MarketDataPublisher (Phase 5) subscribes to a TradeSink and forwards
// relevant events as snapshots / incremental updates.

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace tt {

// Events are simple value types. They share a header for sequencing and
// timestamping.
enum class EventKind : uint8_t {
    OrderAccepted    = 0,
    OrderRested      = 1,
    OrderRejected    = 2,
    OrderCancelled   = 3,
    OrderFilled      = 4,
    OrderPartiallyFilled = 5,
    OrderModified    = 6,
    Trade            = 7,
    TopOfBookChanged = 8,
};

struct OrderPayload {
    OrderId      order_id{kInvalidOrderId};
    TraderId     trader_id{0};
    InstrumentId instrument{kInvalidInstrumentId};
    Side         side{Side::Buy};
    OrderType    type{OrderType::Limit};
    Price        price{Price{0}};
    Quantity     qty{Quantity{0}};
    // Reject reason lives outside the union because std::string is not
    // trivially copyable. Use the trailing-reason scheme: events that need
    // a reason set reject_reason via the helper constructors below.
};

struct Event {
    EventKind  kind{EventKind::OrderAccepted};
    Sequence   sequence{0};
    Timestamp  timestamp{0};

    OrderPayload order{};
    Trade        trade{};
    struct {
        InstrumentId instrument{kInvalidInstrumentId};
        Price        bid_price{Price{0}};
        Quantity     bid_qty{Quantity{0}};
        Price        ask_price{Price{0}};
        Quantity     ask_qty{Quantity{0}};
    } tob{};

    // Only used when kind == OrderRejected.
    std::string  reject_reason;
};

// Helper to construct trade events inline.
inline Event make_trade_event(const Trade& t) {
    Event e{};
    e.kind      = EventKind::Trade;
    e.sequence  = t.sequence;
    e.timestamp = t.timestamp;
    e.trade     = t;
    return e;
}

inline Event make_reject_event(OrderId id, Sequence seq, Timestamp ts,
                               std::string reason) {
    Event e{};
    e.kind            = EventKind::OrderRejected;
    e.sequence        = seq;
    e.timestamp       = ts;
    e.order.order_id  = id;
    e.reject_reason   = std::move(reason);
    return e;
}

// Lightweight best-quote snapshot.
struct TopOfBookSnapshot {
    InstrumentId instrument{kInvalidInstrumentId};
    bool         has_bid{false};
    Price        bid_price{Price{0}};
    Quantity     bid_qty{Quantity{0}};
    bool         has_ask{false};
    Price        ask_price{Price{0}};
    Quantity     ask_qty{Quantity{0}};
};

}  // namespace tt
