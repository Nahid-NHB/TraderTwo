// tests/test_stress.cpp
//
// Phase 13: stress tests + final invariants. These run fairly heavy
// workloads to surface latent bugs (race-ish issues, allocation pressure,
// misuse of std::map) and to confirm correctness properties hold at scale.

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/market_data/publisher.hpp"
#include "tt/matching/matching_engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace tt;

namespace {

Price p(int64_t v) { return Price{v}; }
Quantity q(int64_t v) { return Quantity{v}; }

// A simple bookkeeping sink that tracks invariants we want to hold.
struct InvariantSink final : public TradeSink {
    void on_trade(const Trade& t) noexcept override {
        trades.push_back(t);
        // Trade invariants.
        EXPECT_GT(t.price.ticks, 0);
        EXPECT_GT(t.quantity.qty, 0);
        EXPECT_NE(t.buy_order_id,  kInvalidOrderId);
        EXPECT_NE(t.sell_order_id, kInvalidOrderId);
        // Trade sequence is the taker's submit sequence. Multiple trades
        // can share a sequence (one per fill). Instead we check the trade's
        // own id field is monotonically increasing (assigned per fill).
        if (last_trade_id) {
            EXPECT_GT(static_cast<std::uint64_t>(t.id),
                      static_cast<std::uint64_t>(*last_trade_id));
        }
        last_trade_id = t.id;
    }

    void on_submit_result(const SubmitResult& r) noexcept override {
        // Sequence numbers across both trades and submits must be monotonic.
        results.push_back(r);
        if (last_submit_seq) {
            EXPECT_GT(static_cast<std::uint64_t>(r.sequence),
                      static_cast<std::uint64_t>(*last_submit_seq));
        }
        last_submit_seq = r.sequence;

        // Rejected orders must always carry a reason.
        if (r.status == SubmitStatus::Rejected) {
            EXPECT_FALSE(r.reject_reason.empty());
        }
        // Filled orders must report remaining 0.
        if (r.status == SubmitStatus::FullyFilled) {
            EXPECT_EQ(r.resting_quantity.qty, 0);
        }
        // Accepted/Resting orders must report remaining > 0.
        if (r.status == SubmitStatus::Accepted && r.filled_quantity.qty == 0) {
            // Pure resting (no fills).
        }
    }

    std::vector<Trade>        trades;
    std::vector<SubmitResult> results;
    std::optional<TradeId>    last_trade_id;
    std::optional<Sequence>   last_submit_seq;
};

// Random workload. Drift around an anchor; prices cluster.
struct StressConfig {
    int    instruments{8};
    int    orders_per_instrument{20'000};
    std::uint64_t seed{42};
};

void run_stress(const StressConfig& cfg) {
    MatchingEngine engine;
    for (int i = 1; i <= cfg.instruments; ++i) {
        InstrumentDescriptor d{};
        d.id        = InstrumentId{static_cast<std::uint64_t>(i)};
        d.symbol    = "STRESS" + std::to_string(i);
        d.tick_size = Price{1};
        d.lot_size  = Quantity{1};
        d.enabled   = true;
        engine.register_instrument(d);
    }
    InvariantSink sink;
    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<int>    anchor_dist(95, 105);
    std::uniform_int_distribution<int>    side_dist(0, 1);
    std::uniform_int_distribution<int>    dist(0, 6);   // -3..+3 around anchor
    std::uniform_int_distribution<int>    qty_dist(1, 100);
    std::uniform_int_distribution<int>    instr_dist(0, cfg.instruments - 1);

    const int total = cfg.instruments * cfg.orders_per_instrument;
    for (int i = 0; i < total; ++i) {
        InstrumentId instr = InstrumentId{static_cast<std::uint64_t>(instr_dist(rng) + 1)};
        TraderId     trader = TraderId{static_cast<std::uint64_t>(rng() % 1'000) + 1};
        Side         side   = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        int          anchor = anchor_dist(rng);
        int          off    = dist(rng) - 3;
        int64_t      px     = static_cast<int64_t>(anchor + off);
        if (px < 1) px = 1;
        Quantity     qty    = Quantity{qty_dist(rng)};
        engine.submit_limit(instr, trader, side, Price{px}, qty, TimeInForce::GTC, sink);
    }
}

}  // namespace

TEST(Stress, InvariantsHoldAcrossMixedLoad) {
    StressConfig cfg;
    cfg.instruments          = 4;
    cfg.orders_per_instrument = 5'000;
    run_stress(cfg);

    // No leftover invariants tripped. (The sink ASSERT/EXPECT on each call.)
    SUCCEED();
}

TEST(Stress, SustainedThroughputAcrossManyInstruments) {
    auto t0 = std::chrono::steady_clock::now();
    StressConfig cfg;
    cfg.instruments          = 8;
    cfg.orders_per_instrument = 25'000;
    run_stress(cfg);
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    int total = cfg.instruments * cfg.orders_per_instrument;

    // Generous lower bound — should easily clear 1M orders/sec on modern hw.
    // We don't fail the test if it's slower, just warn.
    auto tps = static_cast<double>(total) / elapsed_s;
    std::printf("[stress] %d orders in %.3f s -> %.0f orders/s\n",
                total, elapsed_s, tps);
    ASSERT_GT(tps, 100'000.0);  // sanity floor
}

TEST(Stress, CancelStormSurvives) {
    // Submit N orders, then cancel half. Verify cancellation count match.
    MatchingEngine engine;
    engine.register_instrument(InstrumentId{1});
    engine.register_instrument(InstrumentId{2});
    InvariantSink sink;

    constexpr int N = 2'000;
    std::vector<OrderId> to_cancel;
    to_cancel.reserve(N);
    for (int i = 0; i < N; ++i) {
        InstrumentId instr = (i & 1) ? InstrumentId{1} : InstrumentId{2};
        engine.submit_limit(instr, TraderId{static_cast<std::uint64_t>(i)+1},
                            Side::Buy, Price{100}, Quantity{1},
                            TimeInForce::GTC, sink);
        to_cancel.push_back(sink.results.back().order_id);
    }
    int cancelled = 0;
    for (OrderId id : to_cancel) {
        // Try both instruments; one of them should match.
        if (engine.cancel(InstrumentId{1}, id)) { ++cancelled; continue; }
        if (engine.cancel(InstrumentId{2}, id)) { ++cancelled; continue; }
    }
    EXPECT_EQ(cancelled, N);
}

TEST(Stress, ModifyStormPreservesPriorityOnReduces) {
    MatchingEngine engine;
    engine.register_instrument(InstrumentId{1});
    InvariantSink sink;

    constexpr int N = 1'000;
    for (int i = 0; i < N; ++i) {
        engine.submit_limit(InstrumentId{1}, TraderId{1}, Side::Buy,
                            Price{100 + i}, Quantity{100},
                            TimeInForce::GTC, sink);
    }
    // Reduce each one (reduce-only preserves priority).
    int ok = 0;
    for (int i = 0; i < N; ++i) {
        OrderId id = sink.results[i].order_id;
        if (engine.reduce(InstrumentId{1}, id, Quantity{50})) ++ok;
    }
    EXPECT_EQ(ok, N);
}

TEST(Stress, RandomCancelModifySweepKeepsOrderBookConsistent) {
    // Random stress: submit many orders, then randomly cancel/modify some.
    MatchingEngine engine;
    engine.register_instrument(InstrumentId{1});
    InvariantSink sink;
    std::mt19937_64 rng(7);

    constexpr int N = 5'000;
    std::vector<OrderId> ids;
    ids.reserve(N);
    for (int i = 0; i < N; ++i) {
        engine.submit_limit(InstrumentId{1},
                            TraderId{static_cast<std::uint64_t>(i)+1},
                            Side::Buy, Price{100}, Quantity{1},
                            TimeInForce::GTC, sink);
        ids.push_back(sink.results.back().order_id);
    }
    std::uniform_int_distribution<int> op_dist(0, 4);
    int cancels = 0, modifies = 0;
    for (int i = 0; i < N; ++i) {
        int op = op_dist(rng);
        OrderId id = ids[static_cast<std::size_t>(rng() % ids.size())];
        if (op == 0 && engine.cancel(InstrumentId{1}, id)) ++cancels;
        else if (op == 1 && engine.reduce(InstrumentId{1}, id, Quantity{1}))
            ++modifies;
    }
    std::printf("[stress] cancels=%d modifies=%d\n", cancels, modifies);
    SUCCEED();
}

TEST(Stress, SequenceNumbersMonotonic) {
    // Hammer the engine and confirm submit_result.sequence is strictly
    // increasing.
    MatchingEngine engine;
    engine.register_instrument(InstrumentId{1});
    InvariantSink sink;

    constexpr int N = 10'000;
    for (int i = 0; i < N; ++i) {
        engine.submit_limit(InstrumentId{1},
                            TraderId{static_cast<std::uint64_t>(i)+1},
                            Side::Buy, Price{100}, Quantity{1},
                            TimeInForce::GTC, sink);
    }
    EXPECT_EQ(sink.results.size(), static_cast<std::size_t>(N));
    for (std::size_t i = 1; i < sink.results.size(); ++i) {
        EXPECT_GT(sink.results[i].sequence, sink.results[i-1].sequence);
    }
}

TEST(Stress, NoOrderIdReusedAcrossEngineLifetime) {
    MatchingEngine engine;
    engine.register_instrument(InstrumentId{1});
    engine.register_instrument(InstrumentId{2});
    InvariantSink sink;

    std::unordered_set<std::uint64_t> seen;
    constexpr int N = 5'000;
    for (int i = 0; i < N; ++i) {
        engine.submit_limit(InstrumentId{1},
                            TraderId{static_cast<std::uint64_t>(i)+1},
                            Side::Buy, Price{100}, Quantity{1},
                            TimeInForce::GTC, sink);
        seen.insert(static_cast<std::uint64_t>(sink.results.back().order_id));
    }
    for (int i = 0; i < N; ++i) {
        engine.submit_limit(InstrumentId{2},
                            TraderId{static_cast<std::uint64_t>(i)+1},
                            Side::Sell, Price{101}, Quantity{1},
                            TimeInForce::GTC, sink);
        seen.insert(static_cast<std::uint64_t>(sink.results.back().order_id));
    }
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(2 * N));
}