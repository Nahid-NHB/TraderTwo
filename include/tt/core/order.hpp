// include/tt/core/order.hpp
//
// Core order representation shared by the matching engine, networking layer
// and tests. This struct is the single source of truth for the *current*
// state of an order while it lives in the book.

#pragma once

#include "tt/common/types.hpp"

#include <cstring>

namespace tt {

enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1,
};

enum class OrderType : uint8_t {
    Market = 0,
    Limit  = 1,
};

enum class TimeInForce : uint8_t {
    // Phase 1 only needs GTC (good-till-cancel). IOC/FOK arrive in a later
    // phase but we keep the enum so the on-the-wire size is fixed.
    GTC = 0,
    IOC = 1,
    FOK = 2,
};

enum class OrderStatus : uint8_t {
    New            = 0,  // accepted, sitting in the book
    PartiallyFilled = 1,
    Filled         = 2,
    Cancelled      = 3,
    Rejected       = 4,
    Expired        = 5,
};

// Tiny self-contained intrusive list node. The order book keeps a doubly
// linked list of Orders per price level; using intrusive links means cancel
// is O(1) without juggling external list iterators.
struct OrderListNode {
    OrderListNode* prev{nullptr};
    OrderListNode* next{nullptr};
};

inline bool operator==(Side a, Side b) noexcept { return static_cast<uint8_t>(a) == static_cast<uint8_t>(b); }
inline bool operator!=(Side a, Side b) noexcept { return !(a == b); }

// Flip a side. Encapsulated as a function so callers don't sprinkle casts.
constexpr Side opposite(Side s) noexcept {
    return static_cast<Side>(static_cast<uint8_t>(s) ^ 1u);
}

#pragma pack(push, 1)
// Packed so the hot-path order stays small. ~64 bytes fits comfortably in a
// single cache line on the order fields that the matcher actually reads.
struct Order {
    // ---- Identity ----------------------------------------------------------
    OrderId      id{kInvalidOrderId};
    TraderId     trader_id{0};
    InstrumentId instrument_id{kInvalidInstrumentId};

    // ---- Static fields -----------------------------------------------------
    Side        side{Side::Buy};
    OrderType   type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};

    // ---- Mutable state -----------------------------------------------------
    Price      price{Price{0}};         // ignored for Market, but stored for simplicity
    Quantity   quantity{Quantity{0}};   // original quantity
    Quantity   remaining{Quantity{0}};  // quantity still working in the book
    Sequence   sequence{0};             // tie-breaker for time priority
    Timestamp  timestamp{0};            // wall-clock at acceptance (informational)
    OrderStatus status{OrderStatus::New};

    // ---- Intrusive list pointers ------------------------------------------
    OrderListNode list_node{};

    // ---- Convenience -------------------------------------------------------
    [[nodiscard]] bool is_limit()  const noexcept { return type == OrderType::Limit;  }
    [[nodiscard]] bool is_market() const noexcept { return type == OrderType::Market; }
    [[nodiscard]] bool is_buy()    const noexcept { return side  == Side::Buy;        }
    [[nodiscard]] bool is_sell()   const noexcept { return side  == Side::Sell;       }

    [[nodiscard]] bool is_open() const noexcept {
        return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
    }
};
#pragma pack(pop)

// ---- Trade -----------------------------------------------------------------
struct Trade {
    TradeId      id{kInvalidTradeId};
    InstrumentId instrument_id{kInvalidInstrumentId};
    OrderId      buy_order_id{kInvalidOrderId};
    OrderId      sell_order_id{kInvalidOrderId};
    TraderId     buy_trader_id{0};
    TraderId     sell_trader_id{0};
    Price        price{Price{0}};
    Quantity     quantity{Quantity{0}};
    Sequence     sequence{0};   // sequence of the incoming (taker) order
    Timestamp    timestamp{0};
};

// ---- Events ----------------------------------------------------------------
// Used by the event publisher and the persistence log. Variant-of-struct
// keeps allocations to zero and gives us a fixed-size event record for the
// append-only log.
struct EventHeader {
    Sequence  sequence{0};
    Timestamp timestamp{0};
};

}  // namespace tt
