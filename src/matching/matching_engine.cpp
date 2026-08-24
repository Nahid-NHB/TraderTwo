// src/matching/matching_engine.cpp
//
// Implementation of the deterministic single-threaded matching engine.

#include "tt/matching/matching_engine.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

namespace tt {

namespace {
inline Timestamp now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<Timestamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

MatchingEngine::MatchingEngine()  = default;
MatchingEngine::~MatchingEngine() = default;

void MatchingEngine::register_instrument(InstrumentId id) {
    if (books_.find(id) == books_.end()) {
        books_.emplace(id, std::make_unique<OrderBook>(id));
    }
}

void MatchingEngine::clear() {
    books_.clear();
    next_sequence_ = 1;
}

OrderBook* MatchingEngine::book(InstrumentId id) noexcept {
    auto it = books_.find(id);
    return it == books_.end() ? nullptr : it->second.get();
}

const OrderBook* MatchingEngine::book(InstrumentId id) const noexcept {
    auto it = books_.find(id);
    return it == books_.end() ? nullptr : it->second.get();
}

void MatchingEngine::submit_limit(InstrumentId instrument, TraderId trader, Side side,
                                  Price price, Quantity qty, TimeInForce tif,
                                  TradeSink& sink) {
    auto o = std::make_unique<Order>();
    o->id            = /*assigned below*/ 0;
    o->trader_id     = trader;
    o->instrument_id = instrument;
    o->side          = side;
    o->type          = OrderType::Limit;
    o->tif           = tif;
    o->price         = price;
    o->quantity      = qty;
    o->remaining     = qty;
    o->timestamp     = now_ns();
    submit(std::move(o), sink);
}

void MatchingEngine::submit(std::unique_ptr<Order> order, TradeSink& sink) {
    SubmitResult result{};

    // ---- Validation ---------------------------------------------------------
    if (!order) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "null order";
        sink.on_submit_result(result);
        return;
    }
    if (order->id == kInvalidOrderId) {
        // Auto-assign from sequence. Real exchanges allocate IDs at ingress
        // (gateway); here we do it at the engine so single-thread tests work.
        order->id = next_sequence_;
    }
    if (order->instrument_id == kInvalidInstrumentId) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "invalid instrument";
        result.order_id = order->id;
        sink.on_submit_result(result);
        return;
    }
    if (!order->quantity.is_valid() || order->remaining.qty <= 0) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "invalid quantity";
        result.order_id = order->id;
        sink.on_submit_result(result);
        return;
    }
    if (order->is_limit() && !order->price.is_valid()) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "invalid price";
        result.order_id = order->id;
        sink.on_submit_result(result);
        return;
    }

    OrderBook* ob = book(order->instrument_id);
    if (!ob) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "instrument not registered";
        result.order_id = order->id;
        sink.on_submit_result(result);
        return;
    }

    // ---- Sequence assignment ------------------------------------------------
    order->sequence = next_sequence_++;
    result.order_id = order->id;
    result.sequence = order->sequence;

    // We release the order from the unique_ptr only when it has been fully
    // consumed or needs to rest. Until then we work with a raw pointer.
    Order* taker = order.get();

    // ---- Match loop ---------------------------------------------------------
    bool still_has_remaining = match_against(*taker, sink);

    if (!still_has_remaining) {
        result.filled_quantity = taker->quantity;
        result.resting_quantity = Quantity{0};
        result.status = SubmitStatus::FullyFilled;
        // Drop the unique_ptr — destroys the Order.
        order.reset();
        sink.on_submit_result(result);
        return;
    }

    // ---- Resting ------------------------------------------------------------
    // Partial or no fill. Decide based on type and TIF.
    if (taker->is_market()) {
        // Market orders don't rest. If there's still qty left, reject the
        // remainder (we still report what we filled).
        result.filled_quantity = Quantity{taker->quantity.qty - taker->remaining.qty};
        result.resting_quantity = Quantity{0};
        result.status = result.filled_quantity.qty > 0 ? SubmitStatus::PartiallyFilled
                                                       : SubmitStatus::Rejected;
        if (result.status == SubmitStatus::Rejected) {
            result.reject_reason = "no liquidity for market order";
        }
        order.reset();
        sink.on_submit_result(result);
        return;
    }

    if (taker->tif == TimeInForce::IOC) {
        // Cancel the unfilled portion.
        result.filled_quantity = Quantity{taker->quantity.qty - taker->remaining.qty};
        result.resting_quantity = Quantity{0};
        result.status = result.filled_quantity.qty > 0 ? SubmitStatus::PartiallyFilled
                                                       : SubmitStatus::Cancelled;
        order.reset();
        sink.on_submit_result(result);
        return;
    }

    // GTC limit: rest the residual.
    rest(*taker);
    result.filled_quantity = Quantity{taker->quantity.qty - taker->remaining.qty};
    result.resting_quantity = taker->remaining;
    if (result.filled_quantity.qty > 0) {
        result.status = SubmitStatus::Accepted;  // partial+resting
    } else {
        result.status = SubmitStatus::Accepted;
    }
    // Transfer ownership to the book.
    bool inserted = ob->insert(std::move(order));
    if (!inserted) {
        // Duplicate ID or other issue — should not happen given validation,
        // but guard anyway.
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "duplicate order id";
    }
    sink.on_submit_result(result);
}

bool MatchingEngine::match_against(Order& taker, TradeSink& sink) {
    OrderBook* ob = book(taker.instrument_id);
    if (!ob) return taker.remaining.qty > 0;

    while (taker.remaining.qty > 0) {
        Price best_opposite = ob->best_opposite_price(taker.side);
        if (!best_opposite.is_valid()) break;

        // Limit price guard.
        if (taker.is_limit()) {
            if (!crosses(taker.side, taker.price, best_opposite)) break;
        }

        // Fill against the front of the opposite book. We must grab the
        // resting order's id before the call because reduce_front may
        // unlink it.
        Order* resting = ob->best_opposite_order(taker.side);
        if (!resting) break;

        OrderId resting_id = resting->id;
        Price   exec_price = best_opposite;  // passive price wins

        // Cap qty at min(taker.remaining, resting.remaining). The book's
        // fill caps at resting.remaining for safety.
        Quantity qty_to_fill{taker.remaining};
        if (qty_to_fill.qty > resting->remaining.qty) {
            qty_to_fill = resting->remaining;
        }

        // Apply the fill to the resting side.
        ob->fill(resting_id, qty_to_fill);
        taker.remaining -= qty_to_fill;

        // Emit trade.
        Trade t{};
        t.id            = next_sequence_++;
        t.instrument_id = taker.instrument_id;
        t.price         = exec_price;
        t.quantity      = qty_to_fill;
        t.sequence      = taker.sequence;
        t.timestamp     = now_ns();
        if (taker.is_buy()) {
            t.buy_order_id  = taker.id;
            t.buy_trader_id = taker.trader_id;
            t.sell_order_id = resting_id;
            t.sell_trader_id= resting->trader_id;
        } else {
            t.sell_order_id = taker.id;
            t.sell_trader_id= taker.trader_id;
            t.buy_order_id  = resting_id;
            t.buy_trader_id = resting->trader_id;
        }
        sink.on_trade(t);
    }

    return taker.remaining.qty > 0;
}

void MatchingEngine::rest(Order& taker) {
    (void)taker;
    // Book insertion happens in submit() after this returns, so we keep the
    // hook in case we need pre-rest validation later (e.g. self-match
    // prevention). Currently a no-op.
}

bool MatchingEngine::cancel(InstrumentId instrument, OrderId id) {
    OrderBook* ob = book(instrument);
    if (!ob) return false;
    return ob->cancel(id);
}

}  // namespace tt
