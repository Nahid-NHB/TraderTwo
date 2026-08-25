// include/tt/market_data/publisher.hpp
//
// Market data publisher. Subscribes to the matching engine by implementing
// TradeSink and translating fill/cancel/resting events into:
//   - TopOfBookSnapshot updates per instrument (incremental)
//   - Depth snapshots on demand
//   - A bounded ring of the last N events per instrument for late joiners
//
// This class is NOT thread-safe by itself. The matching engine calls into
// it on the matching thread. Wrap with a mutex if consumed from another
// thread (the Phase 9 worker pool design ensures only the matching thread
// touches it).

#pragma once

#include "tt/core/order.hpp"
#include "tt/market_data/events.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/orderbook/order_book.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tt {

// Listener interface for market data subscribers (network thread, GUI, etc.).
class MarketDataListener {
public:
    virtual ~MarketDataListener() = default;
    virtual void on_event(const Event& e) noexcept = 0;
    virtual void on_top_of_book(const TopOfBookSnapshot& tob) noexcept = 0;
};

// A trade sink that also publishes market data side effects.
class MarketDataPublisher final : public TradeSink {
public:
    explicit MarketDataPublisher(MatchingEngine& engine) noexcept
        : engine_(engine) {}

    void on_trade(const Trade& t) noexcept override;
    void on_submit_result(const SubmitResult& r) noexcept override;

    // Inspect latest top-of-book snapshot.
    TopOfBookSnapshot top_of_book(InstrumentId id) const;

    // Inspect full depth (top-N levels on each side).
    std::vector<OrderBook::DepthLevel> bid_depth(InstrumentId id,
                                                std::size_t levels) const;
    std::vector<OrderBook::DepthLevel> ask_depth(InstrumentId id,
                                                std::size_t levels) const;

    // Register/unregister a listener. Listeners are called under the
    // matching thread, so they must not block.
    void subscribe(MarketDataListener* l) noexcept;
    void unsubscribe(MarketDataListener* l) noexcept;

    // Last N events recorded.
    const std::vector<Event>& recent_events(std::size_t max_n = 1024) const;

private:
    void notify_event(const Event& e) noexcept;
    void notify_tob(InstrumentId id) noexcept;
    static TopOfBookSnapshot snapshot_of(const OrderBook* book) noexcept;

    MatchingEngine& engine_;

    // Latest snapshot per instrument.
    std::unordered_map<InstrumentId, TopOfBookSnapshot> snapshots_;

    // Bounded ring buffer of recent events.
    static constexpr std::size_t kEventRingCapacity = 4096;
    std::array<Event, kEventRingCapacity> ring_{};
    std::size_t ring_head_{0};
    std::size_t ring_count_{0};
    mutable std::mutex ring_mtx_;

    std::vector<MarketDataListener*> listeners_;
    mutable std::mutex listeners_mtx_;
};

}  // namespace tt
