// include/tt/orderbook/order_book.hpp
//
// Price-time priority order book for a single instrument.
//
// Data structures
// ----------------
//   bids_: std::map<Price, Level*, std::greater<Price>>
//          Bids sorted descending — best bid is bids_.begin().
//
//   asks_: std::map<Price, Level*, std::less<Price>>
//          Asks sorted ascending — best ask is asks_.begin().
//
//   levels_: unordered_map keyed by Price* -> owns the Level nodes so memory
//            management is centralised.
//
//   id_index_: unordered_map<OrderId, Order*> — O(1) cancel/modify by ID.
//
//   Each Level holds an intrusive doubly-linked list of Orders. Insert at
//   tail, remove from middle in O(1) (cancel). The matcher (Phase 2) will
//   walk the head.
//
// Concurrency
// -----------
//   This class is *not* thread-safe. Real exchanges lock per-book. We will
//   add a single mutex around each OrderBook in the worker-thread design in
//   Phase 9; keeping the core logic lock-free makes it trivially testable.

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tt {

// Forward declaration for the Level defined inside OrderBook.
class OrderBook;

// A price level aggregates all orders at a single price. Maintains FIFO order
// via an intrusive doubly-linked list.
class Level {
public:
    Level() = default;
    explicit Level(Price p) : price_(p) {}

    [[nodiscard]] Price price() const noexcept { return price_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return order_count_; }
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }

    void append(Order& o) noexcept {
        // Append at tail to preserve FIFO. The list invariant: head_ is the
        // oldest (highest priority), tail_ is the newest.
        o.list_node.prev = tail_;
        o.list_node.next = nullptr;
        if (tail_) {
            tail_->next = &o.list_node;
        } else {
            head_ = &o.list_node;
        }
        tail_ = &o.list_node;
        ++order_count_;
        total_quantity_ += o.remaining;
    }

    void remove(Order& o) noexcept {
        OrderListNode* node = &o.list_node;
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
        total_quantity_ -= o.remaining;
    }

    [[nodiscard]] Order* head_order() const noexcept {
        return head_ ? container_of(head_, &Order::list_node) : nullptr;
    }

private:
    // Reinterpret an OrderListNode* back to its owning Order*.
    static Order* container_of(OrderListNode* node, OrderListNode Order::* field) noexcept {
        // Compute offset of field within Order at compile time.
        // Using std::launder-equivalent pointer arithmetic: we know the node
        // is embedded at a fixed offset, so we cast through a uintptr_t.
        // Note: rely on offsetof in C++20.
        // (offsetof on a non-standard-layout type is UB pre-C++23; in
        // practice all our compilers accept it on trivially-relocatable PODs.)
        // Fall back to a sentinel nullptr if the offset is wrong.
        static_assert(sizeof(OrderListNode Order::*) > 0, "field pointer must be non-null");
        // Compute offset manually for portability.
        Order temp{};
        std::ptrdiff_t offset =
            reinterpret_cast<char*>(&(temp.*field)) - reinterpret_cast<char*>(&temp);
        return reinterpret_cast<Order*>(reinterpret_cast<char*>(node) - offset);
    }

    Price price_{Price{0}};
    OrderListNode* head_{nullptr};   // oldest order at this level
    OrderListNode* tail_{nullptr};   // newest order at this level
    std::size_t order_count_{0};
    Quantity total_quantity_{Quantity{0}};
};

// The book. Holds orders, supports insert/cancel and best-quote reads.
class OrderBook {
public:
    explicit OrderBook(InstrumentId instrument) noexcept;
    ~OrderBook();

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    [[nodiscard]] InstrumentId instrument_id() const noexcept { return instrument_; }

    // ---- Mutations --------------------------------------------------------
    // Insert a fully-initialised order. Caller must own the Order's storage
    // (e.g. via an arena or unique_ptr). Returns true on success.
    //
    // The book retains a pointer to the Order and will call its destructor
    // when removed/destroyed. The Order must outlive any concurrent access.
    //
    // In Phase 1 we accept ownership via a unique_ptr<Order> stored in
    // id_index_ so callers do not have to think about lifetimes.
    bool insert(std::unique_ptr<Order> order);

    // Cancel an existing order by ID. Returns true if found and cancelled.
    bool cancel(OrderId id);

    // ---- Queries ----------------------------------------------------------
    [[nodiscard]] bool empty() const noexcept {
        return bids_.empty() && asks_.empty();
    }

    [[nodiscard]] Price best_bid() const noexcept;
    [[nodiscard]] Price best_ask() const noexcept;

    [[nodiscard]] Quantity best_bid_quantity() const noexcept;
    [[nodiscard]] Quantity best_ask_quantity() const noexcept;

    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }
    [[nodiscard]] std::size_t total_order_count() const noexcept { return id_index_.size(); }

    // Find an order by ID. Returns nullptr if not found.
    [[nodiscard]] Order* find(OrderId id) const noexcept;

    // Front-of-queue at the best bid/ask — used by Phase 2 matching.
    [[nodiscard]] Order* best_bid_order() const noexcept;
    [[nodiscard]] Order* best_ask_order() const noexcept;

    // Depth snapshot: top-N levels on each side. Each entry is (price,
    // total_quantity, order_count).
    struct DepthLevel {
        Price    price{Price{0}};
        Quantity total_quantity{Quantity{0}};
        std::size_t order_count{0};
    };
    void depth(std::size_t levels, std::vector<DepthLevel>& bids_out,
               std::vector<DepthLevel>& asks_out) const;

private:
    Level* get_or_create_level(Price price, bool is_bid);
    void destroy_level(Price price, bool is_bid);

    InstrumentId instrument_;

    // Ascending price map (for asks). Ascending because the best ask is the
    // lowest price.
    std::map<Price, Level*, std::less<Price>>    asks_;

    // Descending price map (for bids). Descending because the best bid is the
    // highest price.
    std::map<Price, Level*, std::greater<Price>> bids_;

    // Set of all allocated Level pointers, used for cleanup.
    std::unordered_map<Price, std::unique_ptr<Level>> level_storage_;

    // Order ownership. unique_ptr keeps destruction in one place.
    std::unordered_map<OrderId, std::unique_ptr<Order>> id_index_;
};

}  // namespace tt
