// include/tt/market_data/recorder.hpp
//
// A simple MarketDataListener that records what it sees. Useful in tests
// and as a building block for persistence (Phase 7).

#pragma once

#include "tt/market_data/events.hpp"
#include "tt/market_data/publisher.hpp"

#include <vector>

namespace tt {

class RecordingListener final : public MarketDataListener {
public:
    void on_event(const Event& e) noexcept override {
        events.push_back(e);
    }
    void on_top_of_book(const TopOfBookSnapshot& tob) noexcept override {
        snapshots.push_back(tob);
    }
    std::vector<Event>              events;
    std::vector<TopOfBookSnapshot>  snapshots;
};

}  // namespace tt
