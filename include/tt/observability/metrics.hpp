// include/tt/observability/metrics.hpp
//
// Phase 12: in-process metrics. The metrics are simple monotonic counters
// and gauges — no external dependency on Prometheus or similar. They are
// designed to be polled cheaply from a metrics endpoint or printed in a
// summary at shutdown.
//
// All counters are 64-bit atomic so they can be incremented from any
// thread (matching thread, network thread, etc.) without locks. They are
// NOT used on the matching hot path; the engine does not touch metrics
// directly — callers wire a MetricsSink into their TradeSink chain.

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/matching/matching_engine.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace tt {

// Categorical outcomes for submissions. Mirrors SubmitStatus but kept as a
// separate type for label-friendly aggregation.
enum class MetricLabel : uint8_t {
    Accepted,
    Filled,
    Partial,
    Cancelled,
    Rejected,
};

// Atomic counter bundle. One instance per instrument (optional) plus a
// global fallback when no per-instrument tracking is needed.
struct CounterBundle {
    std::atomic<std::uint64_t> submits_accepted{0};
    std::atomic<std::uint64_t> submits_filled{0};
    std::atomic<std::uint64_t> submits_partial{0};
    std::atomic<std::uint64_t> submits_cancelled{0};
    std::atomic<std::uint64_t> submits_rejected{0};

    std::atomic<std::uint64_t> trades_total{0};
    std::atomic<std::uint64_t> trade_qty_total{0};
    std::atomic<std::uint64_t> trade_notional_total{0};

    std::atomic<std::uint64_t> cancels_attempted{0};
    std::atomic<std::uint64_t> cancels_succeeded{0};
    std::atomic<std::uint64_t> modifies_attempted{0};
    std::atomic<std::uint64_t> modifies_succeeded{0};

    // A monotonic "last sequence" gauge. Useful to detect replay progress.
    std::atomic<std::uint64_t> last_sequence{0};

    void reset() noexcept {
        submits_accepted.store(0);
        submits_filled.store(0);
        submits_partial.store(0);
        submits_cancelled.store(0);
        submits_rejected.store(0);
        trades_total.store(0);
        trade_qty_total.store(0);
        trade_notional_total.store(0);
        cancels_attempted.store(0);
        cancels_succeeded.store(0);
        modifies_attempted.store(0);
        modifies_succeeded.store(0);
        last_sequence.store(0);
    }

    // Snapshot for output. Returns by value (small struct of integers).
    struct Snapshot {
        std::uint64_t submits_accepted{0};
        std::uint64_t submits_filled{0};
        std::uint64_t submits_partial{0};
        std::uint64_t submits_cancelled{0};
        std::uint64_t submits_rejected{0};
        std::uint64_t trades_total{0};
        std::uint64_t trade_qty_total{0};
        std::uint64_t trade_notional_total{0};
        std::uint64_t cancels_attempted{0};
        std::uint64_t cancels_succeeded{0};
        std::uint64_t modifies_attempted{0};
        std::uint64_t modifies_succeeded{0};
        std::uint64_t last_sequence{0};
    };

    Snapshot snapshot() const noexcept {
        Snapshot s;
        s.submits_accepted     = submits_accepted.load();
        s.submits_filled       = submits_filled.load();
        s.submits_partial      = submits_partial.load();
        s.submits_cancelled    = submits_cancelled.load();
        s.submits_rejected     = submits_rejected.load();
        s.trades_total         = trades_total.load();
        s.trade_qty_total      = trade_qty_total.load();
        s.trade_notional_total = trade_notional_total.load();
        s.cancels_attempted    = cancels_attempted.load();
        s.cancels_succeeded    = cancels_succeeded.load();
        s.modifies_attempted   = modifies_attempted.load();
        s.modifies_succeeded   = modifies_succeeded.load();
        s.last_sequence        = last_sequence.load();
        return s;
    }
};

// TradeSink that aggregates counters. Construct it with the CounterBundle
// you want it to update. The sink chains downstream so we can layer
// persistence + stdout + metrics without re-implementing logic.
class MetricsSink final : public TradeSink {
public:
    explicit MetricsSink(CounterBundle& bundle, TradeSink* downstream = nullptr) noexcept
        : bundle_(bundle), downstream_(downstream) {}

    void on_trade(const Trade& t) noexcept override;
    void on_submit_result(const SubmitResult& r) noexcept override;

    // Manual hooks for cancel/modify (those don't emit through the trade
    // sink). Use these from the networking or scripting layer.
    void record_cancel_attempt(bool ok) noexcept;
    void record_modify_attempt(bool ok) noexcept;

private:
    CounterBundle& bundle_;
    TradeSink*     downstream_;
};

// Per-instrument counters. Holds a fixed-size array of bundles keyed by
// InstrumentId modulo a cap. For production we'd want a hash map; this is
// sufficient for typical workloads (tens of instruments).
class MetricsRegistry {
public:
    static constexpr std::size_t kMaxInstruments = 256;

    MetricsRegistry() = default;

    CounterBundle& for_instrument(InstrumentId id) noexcept {
        auto slot = static_cast<std::size_t>(id) % kMaxInstruments;
        return per_instrument_[slot];
    }

    const CounterBundle& for_instrument(InstrumentId id) const noexcept {
        auto slot = static_cast<std::size_t>(id) % kMaxInstruments;
        return per_instrument_[slot];
    }

    CounterBundle& global() noexcept { return global_; }

private:
    std::array<CounterBundle, kMaxInstruments> per_instrument_{};
    CounterBundle                             global_;
};

// ---- Text rendering -------------------------------------------------------
// Render a snapshot as a multi-line, Prometheus-style text blob (key/value
// per line). Empty buffer is allowed.
std::string render_prometheus(const CounterBundle::Snapshot& s,
                              const std::string& prefix = "trader_two");

// Render a snapshot as one JSON object on a single line.
std::string render_json(const CounterBundle::Snapshot& s);

}  // namespace tt