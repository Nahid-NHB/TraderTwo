// src/orderbook/order_book.cpp
//
// Implementation of the price-time priority order book.

#include "tt/orderbook/order_book.hpp"

#include <cassert>
#include <utility>

namespace tt {

// ---------------------------------------------------------------------------
// Level
// ---------------------------------------------------------------------------
// Implementation detail. The intrusive helpers live in the header for
// inlining; here we just make sure the container_of helper compiles cleanly.

namespace {
inline Order* order_from_node(OrderListNode* node) noexcept {
    // Compute offset of list_node within Order at compile time via a
    // standard-layout trick: a temporary instance is fine since Order is
    // standard layout (only POD-like fields).
    static_assert(std::is_standard_layout<Order>::value,
                  "Order must be standard-layout for offset arithmetic");
    static constexpr std::ptrdiff_t kOffset =
        reinterpret_cast<char*>(&(reinterpret_cast<Order*>(0x1000)->list_node)) -
        reinterpret_cast<char*>(reinterpret_cast<Order*>(0x1000));
    return reinterpret_cast<Order*>(reinterpret_cast<char*>(node) - kOffset);
}
}  // namespace

// ---------------------------------------------------------------------------
// OrderBook
// ---------------------------------------------------------------------------
OrderBook::OrderBook(InstrumentId instrument) noexcept : instrument_(instrument) {}

OrderBook::~OrderBook() = default;

Level* OrderBook::get_or_create_level(Price price, bool is_bid) {
    auto& ladder = is_bid ? bids_ : asks_;
    auto it = ladder.find(price);
    if (it != ladder.end()) {
        return it->second;
    }
    auto level = std::make_unique<Level>(price);
    Level* raw = level.get();
    level_storage_.emplace(price, std::move(level));
    ladder.emplace(price, raw);
    return raw;
}

void OrderBook::destroy_level(Price price, bool is_bid) {
    auto& ladder = is_bid ? bids_ : asks_;
    ladder.erase(price);
    level_storage_.erase(price);
}

bool OrderBook::insert(std::unique_ptr<Order> order) {
    if (!order) return false;
    if (order->id == kInvalidOrderId) return false;
    if (!order->is_limit()) {
        // Phase 1 only handles resting limit orders. Market orders go through
        // the matcher in Phase 2. We refuse here so the book stays pure.
        return false;
    }
    if (!order->price.is_valid()) return false;
    if (!order->remaining.is_valid()) return false;

    // Reject duplicates so we never have two live orders with the same ID.
    if (id_index_.find(order->id) != id_index_.end()) return false;

    bool is_bid = order->is_buy();
    Level* level = get_or_create_level(order->price, is_bid);

    Order* raw = order.get();
    level->append(*raw);
    id_index_.emplace(raw->id, std::move(order));
    return true;
}

bool OrderBook::cancel(OrderId id) {
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return false;
    Order& o = *(it->second);
    if (o.status == OrderStatus::Filled || o.status == OrderStatus::Cancelled) {
        return false;
    }

    bool is_bid = o.is_buy();
    auto ladder_it = (is_bid ? bids_ : asks_).find(o.price);
    assert(ladder_it != (is_bid ? bids_ : asks_).end());

    Level* lvl = ladder_it->second;
    lvl->remove(o);

    // If the level is now empty, free it.
    if (lvl->order_count() == 0) {
        destroy_level(o.price, is_bid);
    }

    o.status = OrderStatus::Cancelled;
    id_index_.erase(it);
    return true;
}

Price OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return Price{kInvalidPrice};
    return bids_.begin()->first;
}

Price OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return Price{kInvalidPrice};
    return asks_.begin()->first;
}

Quantity OrderBook::best_bid_quantity() const noexcept {
    if (bids_.empty()) return Quantity{0};
    return bids_.begin()->second->total_quantity();
}

Quantity OrderBook::best_ask_quantity() const noexcept {
    if (asks_.empty()) return Quantity{0};
    return asks_.begin()->second->total_quantity();
}

Order* OrderBook::find(OrderId id) const noexcept {
    auto it = id_index_.find(id);
    return it == id_index_.end() ? nullptr : it->second.get();
}

Order* OrderBook::best_bid_order() const noexcept {
    if (bids_.empty()) return nullptr;
    return bids_.begin()->second->head_order();
}

Order* OrderBook::best_ask_order() const noexcept {
    if (asks_.empty()) return nullptr;
    return asks_.begin()->second->head_order();
}

void OrderBook::depth(std::size_t levels,
                      std::vector<DepthLevel>& bids_out,
                      std::vector<DepthLevel>& asks_out) const {
    bids_out.clear();
    asks_out.clear();
    bids_out.reserve(levels);
    asks_out.reserve(levels);

    std::size_t n = 0;
    for (const auto& [price, lvl] : bids_) {
        if (n++ >= levels) break;
        bids_out.push_back(DepthLevel{price, lvl->total_quantity(), lvl->order_count()});
    }
    n = 0;
    for (const auto& [price, lvl] : asks_) {
        if (n++ >= levels) break;
        asks_out.push_back(DepthLevel{price, lvl->total_quantity(), lvl->order_count()});
    }
}

}  // namespace tt
