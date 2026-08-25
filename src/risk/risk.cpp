// src/risk/risk.cpp

#include "tt/risk/risk.hpp"

#include <algorithm>
#include <chrono>

namespace tt {

// ---- MaxOrderQuantityCheck -------------------------------------------------
RiskDecision MaxOrderQuantityCheck::evaluate(const Order& o) const noexcept {
    if (!o.quantity.is_valid()) {
        return RiskDecision::reject(RiskRejectCode::InvalidInput, "invalid quantity");
    }
    if (o.quantity.qty > max_.qty) {
        return RiskDecision::reject(RiskRejectCode::MaxQuantity,
                                    "quantity exceeds maximum");
    }
    return RiskDecision::accept();
}

// ---- MaxNotionalCheck ------------------------------------------------------
RiskDecision MaxNotionalCheck::evaluate(const Order& o) const noexcept {
    if (!o.quantity.is_valid()) {
        return RiskDecision::reject(RiskRejectCode::InvalidInput, "invalid quantity");
    }
    if (!o.price.is_valid()) {
        return RiskDecision::reject(RiskRejectCode::InvalidInput, "invalid price");
    }
    // Use __int128 to avoid overflow.
    __int128 notional = static_cast<__int128>(o.price.ticks) *
                        static_cast<__int128>(o.quantity.qty);
    if (notional > max_ticks_) {
        return RiskDecision::reject(RiskRejectCode::MaxNotional,
                                    "notional exceeds maximum");
    }
    return RiskDecision::accept();
}

// ---- PriceCollarCheck ------------------------------------------------------
RiskDecision PriceCollarCheck::evaluate(const Order& o) const noexcept {
    if (!o.price.is_valid()) {
        return RiskDecision::accept(); // market orders skip price collar
    }
    if (lower_.ticks > 0 && o.price.ticks < lower_.ticks) {
        return RiskDecision::reject(RiskRejectCode::PriceOutOfCollar,
                                    "price below lower collar");
    }
    if (upper_.ticks > 0 && o.price.ticks > upper_.ticks) {
        return RiskDecision::reject(RiskRejectCode::PriceOutOfCollar,
                                    "price above upper collar");
    }
    return RiskDecision::accept();
}

// ---- RateLimitCheck --------------------------------------------------------
namespace {
inline std::int64_t now_ns_steady() {
    using namespace std::chrono;
    return static_cast<std::int64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

RiskDecision RateLimitCheck::evaluate(const Order& o) const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    State& s = states_[o.trader_id];
    std::int64_t now = now_ns_steady();
    if (s.last_ns == 0) {
        s.tokens   = static_cast<double>(burst_);
        s.last_ns  = now;
    } else {
        std::int64_t elapsed = now - s.last_ns;
        if (elapsed > 0) {
            double add = static_cast<double>(elapsed) / static_cast<double>(refill_ns_);
            s.tokens   = std::min<double>(s.tokens + add, static_cast<double>(burst_));
            s.last_ns  = now;
        }
    }
    if (s.tokens < 1.0) {
        return RiskDecision::reject(RiskRejectCode::RateLimited,
                                    "rate limit exceeded");
    }
    s.tokens -= 1.0;
    return RiskDecision::accept();
}

// ---- RiskGate --------------------------------------------------------------
void RiskGate::add_check(std::unique_ptr<RiskCheck> c) {
    checks_.push_back(std::move(c));
}

void RiskGate::clear_checks() {
    checks_.clear();
}

RiskDecision RiskGate::evaluate(const Order& o) const noexcept {
    if (checks_.empty()) return RiskDecision::accept();
    for (const auto& c : checks_) {
        auto d = c->evaluate(o);
        if (!d.ok()) return d;
    }
    return RiskDecision::accept();
}

}  // namespace tt
