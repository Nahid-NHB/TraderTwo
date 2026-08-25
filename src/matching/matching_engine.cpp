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
    if (descriptors_.find(id) == descriptors_.end()) {
        InstrumentDescriptor d{};
        d.id        = id;
        d.symbol    = std::to_string(id);
        d.tick_size = Price{1};
        d.lot_size  = Quantity{1};
        d.enabled   = true;
        descriptors_.emplace(id, d);
    }
}

void MatchingEngine::register_instrument(const InstrumentDescriptor& desc) {
    register_instrument(desc.id);
    descriptors_[desc.id] = desc;
}

bool MatchingEngine::unregister_instrument(InstrumentId id) {
    auto it = books_.find(id);
    if (it == books_.end()) return false;
    books_.erase(it);
    descriptors_.erase(id);
    return true;
}

const InstrumentDescriptor* MatchingEngine::descriptor(InstrumentId id) const noexcept {
    auto it = descriptors_.find(id);
    return it == descriptors_.end() ? nullptr : &it->second;
}

std::vector<InstrumentId> MatchingEngine::instrument_ids() const {
    std::vector<InstrumentId> out;
    out.reserve(descriptors_.size());
    for (const auto& [id, _] : descriptors_) out.push_back(id);
    return out;
}

std::string MatchingEngine::validate_order(InstrumentId id, Price price,
                                           Quantity qty) const noexcept {
    if (!qty.is_valid()) return "invalid quantity";
    const auto* desc = descriptor(id);
    if (!desc) return "instrument not registered";
    if (!desc->enabled) return "instrument disabled";
    if (price.ticks > 0) {
        if (!is_valid_tick(desc->tick_size)) return "invalid tick size";
        if (!price.is_valid()) return "invalid price";
        if ((price.ticks % desc->tick_size.ticks) != 0) return "price not on tick";
    }
    if (!is_valid_lot(desc->lot_size)) return "invalid lot size";
    if ((qty.qty % desc->lot_size.qty) != 0) return "qty not on lot";
    return {};
}

void MatchingEngine::clear() {
    books_.clear();
    descriptors_.clear();
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

void MatchingEngine::set_risk_gate(std::shared_ptr<RiskGate> gate) noexcept {
    risk_gate_ = std::move(gate);
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
    result.order_id      = order->id;
    result.instrument_id = order->instrument_id;

    if (order->instrument_id == kInvalidInstrumentId) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "invalid instrument";
        sink.on_submit_result(result);
        return;
    }
    if (!order->quantity.is_valid() || order->remaining.qty <= 0) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "invalid quantity";
        sink.on_submit_result(result);
        return;
    }
    if (order->is_limit() && !order->price.is_valid()) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "invalid price";
        sink.on_submit_result(result);
        return;
    }

    OrderBook* ob = book(order->instrument_id);
    if (!ob) {
        result.status = SubmitStatus::Rejected;
        result.reject_reason = "instrument not registered";
        sink.on_submit_result(result);
        return;
    }

    // ---- Risk gate -----------------------------------------------------------
    if (risk_gate_) {
        RiskDecision d = risk_gate_->evaluate(*order);
        if (!d.ok()) {
            result.status = SubmitStatus::Rejected;
            result.reject_reason = "risk: " + d.reason;
            sink.on_submit_result(result);
            return;
        }
    }

    // ---- Sequence assignment ------------------------------------------------
    order->sequence = next_sequence_++;
    result.sequence = order->sequence;
    result.side     = order->side;
    result.type     = order->type;
    result.price    = order->price;

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
    if (!ob) [[unlikely]] return taker.remaining.qty > 0;

    while (taker.remaining.qty > 0) [[likely]] {
        Price best_opposite = ob->best_opposite_price(taker.side);
        if (!best_opposite.is_valid()) [[unlikely]] break;

        // Limit price guard.
        if (taker.is_limit()) [[likely]] {
            if (!crosses(taker.side, taker.price, best_opposite)) [[unlikely]] break;
        }

        // Fill against the front of the opposite book. We must grab the
        // resting order's id before the call because reduce_front may
        // unlink it.
        Order* resting = ob->best_opposite_order(taker.side);
        if (!resting) [[unlikely]] break;

        OrderId resting_id = resting->id;
        Price   exec_price = best_opposite;  // passive price wins

        // Cap qty at min(taker.remaining, resting.remaining). The book's
        // fill caps at resting.remaining for safety.
        Quantity qty_to_fill{taker.remaining};
        if (qty_to_fill.qty > resting->remaining.qty) [[likely]] {
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
        if (taker.is_buy()) [[likely]] {
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

OrderBook::ModifyResult MatchingEngine::modify(InstrumentId instrument,
                                               OrderId id,
                                               Quantity new_qty,
                                               Price new_price) {
    OrderBook* ob = book(instrument);
    if (!ob) return OrderBook::ModifyResult::NotFound;
    return ob->modify(id, new_qty, new_price);
}

bool MatchingEngine::reduce(InstrumentId instrument, OrderId id,
                            Quantity new_qty) {
    OrderBook* ob = book(instrument);
    if (!ob) return false;
    return ob->reduce(id, new_qty);
}

}  // namespace tt
