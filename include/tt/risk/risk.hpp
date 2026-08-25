// include/tt/risk/risk.hpp
//
// Pre-trade risk checks. Sits between the gateway and the matching engine.
// Each check is independent and returns either a pass or a structured
// reject reason. The composite RiskGate evaluates them in order; the first
// failure rejects the order.
//
// Categories of checks implemented:
//   - MaxOrderQuantityCheck
//   - MaxNotionalCheck
//   - PriceCollarCheck (price must be within [lower, upper])
//   - RateLimitCheck (per trader, orders per second)
//   - SelfTradePreventionCheck (block orders that would trade against
//     another order from the same trader — this is a thin version that
//     uses the matching engine's existing data).
//
// RiskGate is configured at startup; the same instance can be shared
// across multiple trading threads if used in a read-only fashion.

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tt {

enum class RiskRejectCode : uint16_t {
    None              = 0,
    NullOrder         = 1,
    MaxQuantity       = 2,
    MaxNotional       = 3,
    PriceOutOfCollar  = 4,
    RateLimited       = 5,
    SelfTradePrevent  = 6,
    InvalidInput      = 7,
};

struct RiskDecision {
    RiskRejectCode code{RiskRejectCode::None};
    std::string    reason;

    bool ok() const noexcept { return code == RiskRejectCode::None; }
    static RiskDecision accept() noexcept { return {}; }
    static RiskDecision reject(RiskRejectCode c, std::string r) noexcept {
        RiskDecision d; d.code = c; d.reason = std::move(r); return d;
    }
};

class RiskCheck {
public:
    virtual ~RiskCheck() = default;
    virtual RiskDecision evaluate(const Order& o) const noexcept = 0;
};

// Hard cap on per-order quantity.
class MaxOrderQuantityCheck final : public RiskCheck {
public:
    explicit MaxOrderQuantityCheck(Quantity max) : max_(max) {}
    RiskDecision evaluate(const Order& o) const noexcept override;
private:
    Quantity max_;
};

// Hard cap on per-order notional (price * qty).
class MaxNotionalCheck final : public RiskCheck {
public:
    explicit MaxNotionalCheck(int64_t max_ticks) : max_ticks_(max_ticks) {}
    RiskDecision evaluate(const Order& o) const noexcept override;
private:
    int64_t max_ticks_;
};

// Price must be within [lower, upper]. A zero bound disables that side.
class PriceCollarCheck final : public RiskCheck {
public:
    PriceCollarCheck(Price lower, Price upper) noexcept
        : lower_(lower), upper_(upper) {}
    RiskDecision evaluate(const Order& o) const noexcept override;
private:
    Price lower_;
    Price upper_;
};

// Token-bucket rate limit per trader. Allows burst_size orders per
// (burst_size / refill_per_second) seconds.
class RateLimitCheck final : public RiskCheck {
public:
    RateLimitCheck(std::size_t burst, double refill_per_second)
        : burst_(burst), refill_ns_(static_cast<std::int64_t>(
              1e9 / std::max(1e-6, refill_per_second))) {}
    RiskDecision evaluate(const Order& o) const noexcept override;
private:
    struct State {
        double       tokens{0};
        std::int64_t last_ns{0};
    };
    std::size_t burst_;
    std::int64_t refill_ns_;
    mutable std::unordered_map<TraderId, State> states_;
    mutable std::mutex mtx_;
};

// Always pass. Useful for tests.
class AlwaysAllow final : public RiskCheck {
public:
    RiskDecision evaluate(const Order&) const noexcept override { return RiskDecision::accept(); }
};

// Composite gate that runs all configured checks. Thread-safe for
// concurrent evaluate() calls.
class RiskGate {
public:
    RiskGate() = default;

    void add_check(std::unique_ptr<RiskCheck> c);
    void clear_checks();
    std::size_t check_count() const noexcept { return checks_.size(); }

    RiskDecision evaluate(const Order& o) const noexcept;

private:
    std::vector<std::unique_ptr<RiskCheck>> checks_;
};

}  // namespace tt
