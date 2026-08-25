// src/market_data/publisher.cpp
//
// Implementation of MarketDataPublisher.

#include "tt/market_data/publisher.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace tt {

void MarketDataPublisher::on_trade(const Trade& t) noexcept {
    Event e = make_trade_event(t);
    notify_event(e);

    // A trade changes top of book.
    notify_tob(t.instrument_id);
}

void MarketDataPublisher::on_submit_result(const SubmitResult& r) noexcept {
    Event e{};
    e.sequence        = r.sequence;
    e.timestamp       = 0;
    e.order.order_id  = r.order_id;
    e.order.qty       = r.resting_quantity;
    switch (r.status) {
        case SubmitStatus::Accepted:
            e.kind = EventKind::OrderRested;
            break;
        case SubmitStatus::FullyFilled:
            e.kind = EventKind::OrderFilled;
            break;
        case SubmitStatus::PartiallyFilled:
            e.kind = EventKind::OrderPartiallyFilled;
            break;
        case SubmitStatus::Cancelled:
            e.kind = EventKind::OrderCancelled;
            break;
        case SubmitStatus::Rejected:
            e.kind = EventKind::OrderRejected;
            e.reject_reason = r.reject_reason;
            break;
    }
    notify_event(e);

    if (r.status == SubmitStatus::Accepted ||
        r.status == SubmitStatus::PartiallyFilled ||
        r.status == SubmitStatus::FullyFilled) {
        notify_tob(r.instrument_id);
    }
}

TopOfBookSnapshot MarketDataPublisher::snapshot_of(const OrderBook* book) noexcept {
    TopOfBookSnapshot s{};
    if (!book) return s;
    s.instrument = book->instrument_id();
    OrderBook::TopOfBook b = book->top_bid();
    OrderBook::TopOfBook a = book->top_ask();
    if (b.valid) {
        s.has_bid    = true;
        s.bid_price  = b.price;
        s.bid_qty    = b.quantity;
    }
    if (a.valid) {
        s.has_ask    = true;
        s.ask_price  = a.price;
        s.ask_qty    = a.quantity;
    }
    return s;
}

TopOfBookSnapshot MarketDataPublisher::top_of_book(InstrumentId id) const {
    auto it = snapshots_.find(id);
    if (it != snapshots_.end()) return it->second;
    return snapshot_of(engine_.book(id));
}

std::vector<OrderBook::DepthLevel>
MarketDataPublisher::bid_depth(InstrumentId id, std::size_t levels) const {
    std::vector<OrderBook::DepthLevel> out;
    const OrderBook* ob = engine_.book(id);
    if (!ob) return out;
    std::vector<OrderBook::DepthLevel> asks;
    ob->depth(levels, out, asks);
    return out;
}

std::vector<OrderBook::DepthLevel>
MarketDataPublisher::ask_depth(InstrumentId id, std::size_t levels) const {
    std::vector<OrderBook::DepthLevel> out;
    const OrderBook* ob = engine_.book(id);
    if (!ob) return out;
    std::vector<OrderBook::DepthLevel> bids;
    ob->depth(levels, bids, out);
    return out;
}

void MarketDataPublisher::subscribe(MarketDataListener* l) noexcept {
    std::lock_guard<std::mutex> lk(listeners_mtx_);
    if (std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) {
        listeners_.push_back(l);
    }
}

void MarketDataPublisher::unsubscribe(MarketDataListener* l) noexcept {
    std::lock_guard<std::mutex> lk(listeners_mtx_);
    auto it = std::find(listeners_.begin(), listeners_.end(), l);
    if (it != listeners_.end()) listeners_.erase(it);
}

void MarketDataPublisher::notify_event(const Event& e) noexcept {
    {
        std::lock_guard<std::mutex> lk(ring_mtx_);
        ring_[ring_head_] = e;
        ring_head_ = (ring_head_ + 1) % kEventRingCapacity;
        if (ring_count_ < kEventRingCapacity) ++ring_count_;
    }
    std::vector<MarketDataListener*> copy;
    {
        std::lock_guard<std::mutex> lk(listeners_mtx_);
        copy = listeners_;
    }
    for (auto* l : copy) l->on_event(e);
}

void MarketDataPublisher::notify_tob(InstrumentId id) noexcept {
    const OrderBook* ob = engine_.book(id);
    if (!ob) return;
    TopOfBookSnapshot s = snapshot_of(ob);
    snapshots_[id] = s;
    std::vector<MarketDataListener*> copy;
    {
        std::lock_guard<std::mutex> lk(listeners_mtx_);
        copy = listeners_;
    }
    for (auto* l : copy) l->on_top_of_book(s);
}

const std::vector<Event>& MarketDataPublisher::recent_events(std::size_t max_n) const {
    static thread_local std::vector<Event> tmp;
    tmp.clear();
    std::lock_guard<std::mutex> lk(ring_mtx_);
    std::size_t n = std::min(max_n, ring_count_);
    tmp.reserve(n);
    // Walk backwards from the most recent.
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t idx = (ring_head_ + kEventRingCapacity - 1 - i) % kEventRingCapacity;
        tmp.push_back(ring_[idx]);
    }
    return tmp;
}

}  // namespace tt