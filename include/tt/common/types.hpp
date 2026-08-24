// include/tt/common/types.hpp
//
// Fundamental value types used throughout the exchange.
//
// Design notes:
// * Prices are stored as int64_t in fixed-point ticks (see kPriceScale). Using
//   integer math avoids IEEE-754 surprises in the matching hot path and makes
//   equality, ordering and hashing trivially deterministic.
// * Quantities are non-negative int64_t. We rely on the OrderBook/Engine to
//   guard against underflow on subtract.
// * IDs are 64-bit so they can be globally unique across the lifetime of the
//   process without recycling.
// * Sequence numbers are a single, monotonic uint64_t per exchange instance
//   and are the authoritative ordering for price-time priority tie-breaks
//   (wall-clock is allowed to be non-monotonic due to NTP slews).

#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>

namespace tt {

// ---- Fixed-point price scale -----------------------------------------------
// 1 price unit == 1 / kPriceScale dollars. 1_000_000 == $1.000000.
inline constexpr int64_t kPriceScale      = 1'000'000;
inline constexpr int64_t kMaxPriceTicks   = 1'000'000'000'000LL;   // ~$1B in ticks
inline constexpr int64_t kMaxQuantity     = 1'000'000'000'000LL;   // 1T shares
inline constexpr int64_t kInvalidPrice    = std::numeric_limits<int64_t>::min();

// ---- Identifier aliases ----------------------------------------------------
using OrderId      = uint64_t;
using TradeId      = uint64_t;
using TraderId     = uint64_t;
using InstrumentId = uint64_t;
using Sequence     = uint64_t;
using Timestamp    = uint64_t;   // nanoseconds since epoch (monotonic OK)

inline constexpr OrderId      kInvalidOrderId      = 0;
inline constexpr TradeId      kInvalidTradeId      = 0;
inline constexpr InstrumentId kInvalidInstrumentId = 0;

// ---- Strong types via trivial wrappers ------------------------------------
// Using thin wrappers prevents mixing Price and Quantity accidentally. They
// remain implicitly convertible to the underlying scalar so performance stays
// identical to raw int64_t.
struct Price {
    int64_t ticks{kInvalidPrice};

    constexpr Price() = default;
    constexpr explicit Price(int64_t t) noexcept : ticks(t) {}

    [[nodiscard]] constexpr auto operator<=>(const Price&) const noexcept = default;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return ticks >= 0 && ticks <= kMaxPriceTicks;
    }

    // Convert a human decimal (e.g. 100.50) to ticks. Caller is responsible
    // for value range; this is only used at API boundaries, never in hot path.
    static constexpr Price from_double(double d) noexcept {
        return Price(static_cast<int64_t>(d * static_cast<double>(kPriceScale)));
    }

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(ticks) / static_cast<double>(kPriceScale);
    }
};

struct Quantity {
    int64_t qty{0};

    constexpr Quantity() = default;
    constexpr explicit Quantity(int64_t q) noexcept : qty(q) {}

    [[nodiscard]] constexpr auto operator<=>(const Quantity&) const noexcept = default;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return qty > 0 && qty <= kMaxQuantity;
    }

    constexpr Quantity& operator-=(Quantity other) noexcept {
        qty -= other.qty;
        return *this;
    }
    constexpr Quantity& operator+=(Quantity other) noexcept {
        qty += other.qty;
        return *this;
    }
    [[nodiscard]] constexpr Quantity operator-(Quantity other) const noexcept {
        return Quantity(qty - other.qty);
    }
    [[nodiscard]] constexpr Quantity operator+(Quantity other) const noexcept {
        return Quantity(qty + other.qty);
    }
};

// ---- Hash specialisations -------------------------------------------------
// std::hash specialisations so Price/Quantity can be used directly in
// unordered containers without conversion noise.
}  // namespace tt

namespace std {
template <> struct hash<tt::Price> {
    size_t operator()(tt::Price p) const noexcept {
        return std::hash<int64_t>{}(p.ticks);
    }
};
template <> struct hash<tt::Quantity> {
    size_t operator()(tt::Quantity q) const noexcept {
        return std::hash<int64_t>{}(q.qty);
    }
};
}  // namespace std
