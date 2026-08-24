// src/orderbook/order_book.cpp
//
// Implementation of the price-time priority order book.

#include "tt/orderbook/order_book.hpp"

#include <cassert>
#include <type_traits>
#include <utility>

namespace tt {

// ---------------------------------------------------------------------------
// Level
// ---------------------------------------------------------------------------
// Implementation detail. The intrusive helpers live in the header for
// inlining; here we just make sure the container_of helper compiles cleanly.

namespace {
inline Order* order_from_node(OrderListNode* node) noexcept {
    // Order is a packed standard-layout struct, so we can compute the offset
    // of list_node with offsetof. We use a local Order instance rather than
    // dereferencing a literal pointer (which would be UB).
    Order sample{};
    std::ptrdiff_t offset = reinterpret_cast<char*>(&sample.list_node) -
                            reinterpret_cast<char*>(&sample);
    return reinterpret_cast<Order*>(reinterpret_cast<char*>(node) - offset);
}
}  // namespace

// ---------------------------------------------------------------------------
// Level method bodies that need the offset helper.
// ---------------------------------------------------------------------------
Order* Level::head_order() const noexcept {
    return head_ ? order_from_node(head_) : nullptr;
}

// ---------------------------------------------------------------------------
// OrderBook
// ---------------------------------------------------------------------------
OrderBook::OrderBook(InstrumentId instrument) noexcept : instrument_(instrument) {}

OrderBook::~OrderBook() = default;

Level* OrderBook::get_or_create_level(Price price, bool is_bid) {
    if (is_bid) {
        auto it = bids_.find(price);
        if (it != bids_.end()) return it->second;
        auto level = std::make_unique<Level>(price);
        Level* raw = level.get();
        level_storage_.emplace(price, std::move(level));
        bids_.emplace(price, raw);
        return raw;
    }
    auto it = asks_.find(price);
    if (it != asks_.end()) return it->second;
    auto level = std::make_unique<Level>(price);
    Level* raw = level.get();
    level_storage_.emplace(price, std::move(level));
    asks_.emplace(price, raw);
    return raw;
}

void OrderBook::destroy_level(Price price, bool is_bid) {
    if (is_bid) {
        bids_.erase(price);
    } else {
        asks_.erase(price);
    }
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
    if (is_bid) {
        auto ladder_it = bids_.find(o.price);
        assert(ladder_it != bids_.end());
        Level* lvl = ladder_it->second;
        lvl->remove(o);
        if (lvl->order_count() == 0) {
            destroy_level(o.price, true);
        }
    } else {
        auto ladder_it = asks_.find(o.price);
        assert(ladder_it != asks_.end());
        Level* lvl = ladder_it->second;
        lvl->remove(o);
        if (lvl->order_count() == 0) {
            destroy_level(o.price, false);
        }
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

// ---------------------------------------------------------------------------
// Level: match-time helpers
// ---------------------------------------------------------------------------
Quantity Level::front_remaining() const noexcept {
    Order* o = front();
    return o ? o->remaining : Quantity{0};
}

Quantity Level::reduce_front(Quantity qty, Order*& filled_out, bool& consumed_out) noexcept {
    Order* o = front();
    filled_out = nullptr;
    consumed_out = false;
    if (!o || qty.qty <= 0) return Quantity{0};

    Quantity fill_qty = qty;
    if (fill_qty.qty > o->remaining.qty) fill_qty = o->remaining;

    // Update level totals: subtract the fill from total, but keep the
    // residual on the books.
    total_quantity_ -= fill_qty;
    o->remaining    -= fill_qty;

    if (o->remaining.qty == 0) {
        // Fully consumed. Unlink from the list and update counts.
        OrderListNode* node = &o->list_node;
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            head_ = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        } else {
            tail_ = node->prev;
        }
        node->prev = nullptr;
        node->next = nullptr;
        --order_count_;
        consumed_out = true;
        o->status = OrderStatus::Filled;
    } else {
        // Partial fill: residual stays at HEAD (preserves time priority).
        if (o->status == OrderStatus::New) {
            o->status = OrderStatus::PartiallyFilled;
        }
    }
    filled_out = o;
    return fill_qty;
}

// ---------------------------------------------------------------------------
// OrderBook: match-time helpers
// ---------------------------------------------------------------------------
// Apply a fill to a resting order by ID. Decrements remaining, unlinks if
// fully consumed, frees the level if it becomes empty. Returns true if the
// order is still in the book after the call (i.e. partial fill).
bool OrderBook::fill(OrderId id, Quantity qty) noexcept {
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return false;
    Order& o = *(it->second);

    bool is_bid = o.is_buy();
    Level* lvl = nullptr;
    if (is_bid) {
        auto lit = bids_.find(o.price);
        assert(lit != bids_.end());
        lvl = lit->second;
    } else {
        auto lit = asks_.find(o.price);
        assert(lit != asks_.end());
        lvl = lit->second;
    }

    Order* filled = nullptr;
    bool consumed = false;
    // reduce_front caps qty at the front's remaining, so even if the
    // caller passes too much we never over-fill.
    lvl->reduce_front(qty, filled, consumed);

    if (consumed) {
        id_index_.erase(it);
        if (lvl->order_count() == 0) {
            destroy_level(o.price, is_bid);
        }
    }
    return !consumed;
}

void OrderBook::remove(OrderId id) noexcept {
    cancel(id);
}

Order* OrderBook::best_opposite_order(Side aggressor_side) const noexcept {
    return aggressor_side == Side::Buy ? best_ask_order() : best_bid_order();
}

Price OrderBook::best_opposite_price(Side aggressor_side) const noexcept {
    return aggressor_side == Side::Buy ? best_ask() : best_bid();
}

OrderBook::TopOfBook OrderBook::top_bid() const noexcept {
    if (bids_.empty()) return TopOfBook{};
    const Level* lvl = bids_.begin()->second;
    return TopOfBook{true, bids_.begin()->first, lvl->total_quantity(), lvl->order_count()};
}

OrderBook::TopOfBook OrderBook::top_ask() const noexcept {
    if (asks_.empty()) return TopOfBook{};
    const Level* lvl = asks_.begin()->second;
    return TopOfBook{true, asks_.begin()->first, lvl->total_quantity(), lvl->order_count()};
}

}  // namespace tt
