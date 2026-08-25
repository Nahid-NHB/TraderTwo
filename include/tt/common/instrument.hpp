// include/tt/common/instrument.hpp
//
// Instrument metadata + registry. A registered instrument carries:
//   - human-readable symbol
//   - tick size (smallest price increment)
//   - lot size (smallest tradable quantity increment)
//   - display name (optional)
//   - enabled flag
//
// Phase 4 keeps the registry inside MatchingEngine, but exposes a richer
// InstrumentDescriptor so external code (networking, market data) can
// discover what is supported.

#pragma once

#include "tt/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace tt {

struct InstrumentDescriptor {
    InstrumentId id{kInvalidInstrumentId};
    std::string  symbol;            // e.g. "AAPL"
    std::string  display_name;      // e.g. "Apple Inc."
    Price        tick_size{Price{1}};
    Quantity     lot_size{Quantity{1}};
    bool         enabled{true};
};

// ---- Validation helpers ---------------------------------------------------
inline bool is_valid_tick(Price tick) noexcept {
    return tick.ticks > 0 && tick.ticks <= kMaxPriceTicks;
}
inline bool is_valid_lot(Quantity lot) noexcept {
    return lot.qty > 0 && lot.qty <= kMaxQuantity;
}

// Round a price down to the nearest tick multiple.
inline Price round_to_tick(Price p, Price tick) noexcept {
    if (tick.ticks <= 0) return p;
    int64_t t = p.ticks - (p.ticks % tick.ticks);
    return Price{t};
}

// Round a quantity down to the nearest lot multiple.
inline Quantity round_to_lot(Quantity q, Quantity lot) noexcept {
    if (lot.qty <= 0) return q;
    int64_t v = q.qty - (q.qty % lot.qty);
    return Quantity{v};
}

}  // namespace tt
