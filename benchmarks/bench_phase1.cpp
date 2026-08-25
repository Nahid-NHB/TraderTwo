// benchmarks/bench_phase1.cpp
//
// Phase 11: Google-Benchmark scenarios for the matching hot loop.
//
//   BM_Submit_NoCross        Submit N orders that don't cross — pure insertion.
//   BM_Submit_FullCross      Submit N orders that all cross — pure matching.
//   BM_Submit_InsertSameLevel Submit a single order at one price, repeatedly.
//   BM_Modify_Reduce         Reduce a resting order's quantity.
//   BM_Cancel_Head           Cancel the head of a long queue (worst case).
//   BM_SubmitThenCancel      Submit + immediate cancel.
//
// All benchmarks run against a single MatchingEngine on a single instrument
// with an in-process CollectingSink (no I/O, no allocations on the hot path
// beyond Order ownership transfer).

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/matching/matching_engine.hpp"

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace tt;

namespace {

// Fixture: a fresh engine + sink per benchmark iteration.
struct EngineFixture {
    MatchingEngine engine;
    CollectingSink sink;

    EngineFixture() {
        engine.register_instrument(InstrumentId{1});
    }
};

// Submit N orders at a fixed price on the buy side (no cross because the
// book is initially empty). Pure insertion.
static void BM_Submit_NoCross(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    for (auto _ : state) {
        EngineFixture f;
        for (int i = 0; i < N; ++i) {
            f.engine.submit_limit(InstrumentId{1},
                                  TraderId{static_cast<std::uint64_t>(i)+1},
                                  Side::Buy, Price{100}, Quantity{1},
                                  TimeInForce::GTC, f.sink);
        }
    }
    state.SetItemsProcessed(state.iterations() * N);
    state.SetComplexityN(N);
}
BENCHMARK(BM_Submit_NoCross)
    ->RangeMultiplier(4)->Range(1<<6, 1<<14)
    ->Unit(benchmark::kMicrosecond);

// Submit N orders on the buy side at price 105 against an ask book seeded
// at price 100. Each submission fully or partially fills — pure matching.
static void BM_Submit_FullCross(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    for (auto _ : state) {
        EngineFixture f;
        // Seed: 100 sell orders at 100, total 100 qty.
        for (int i = 0; i < N; ++i) {
            f.engine.submit_limit(InstrumentId{1},
                                  TraderId{static_cast<std::uint64_t>(i)+1},
                                  Side::Sell, Price{100}, Quantity{1},
                                  TimeInForce::GTC, f.sink);
        }
        // Now buy: each fill consumes one unit of the resting ask.
        for (int i = 0; i < N; ++i) {
            f.engine.submit_limit(InstrumentId{1},
                                  TraderId{static_cast<std::uint64_t>(1000+i)},
                                  Side::Buy, Price{105}, Quantity{1},
                                  TimeInForce::GTC, f.sink);
        }
    }
    state.SetItemsProcessed(state.iterations() * (2 * N));
    state.SetComplexityN(N);
}
BENCHMARK(BM_Submit_FullCross)
    ->RangeMultiplier(4)->Range(1<<6, 1<<12)
    ->Unit(benchmark::kMicrosecond);

// Insert at the same price level repeatedly (queue growth at one level).
static void BM_Submit_InsertSameLevel(benchmark::State& state) {
    EngineFixture f;
    for (auto _ : state) {
        state.PauseTiming();
        f.engine.clear();
        f.engine.register_instrument(InstrumentId{1});
        state.ResumeTiming();

        f.engine.submit_limit(InstrumentId{1}, TraderId{1}, Side::Buy,
                              Price{100}, Quantity{10}, TimeInForce::GTC, f.sink);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Submit_InsertSameLevel);

// Reduce-only modification: preserve priority, drop quantity.
static void BM_Modify_Reduce(benchmark::State& state) {
    EngineFixture f;
    f.engine.submit_limit(InstrumentId{1}, TraderId{1}, Side::Sell,
                          Price{100}, Quantity{10'000}, TimeInForce::GTC, f.sink);
    OrderId resting_id = f.sink.results.back().order_id;
    Quantity new_qty   = Quantity{5'000};

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            f.engine.reduce(InstrumentId{1}, resting_id, new_qty));
        state.PauseTiming();
        f.engine.clear();
        f.engine.register_instrument(InstrumentId{1});
        f.engine.submit_limit(InstrumentId{1}, TraderId{1}, Side::Sell,
                              Price{100}, Quantity{10'000}, TimeInForce::GTC,
                              f.sink);
        resting_id = f.sink.results.back().order_id;
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Modify_Reduce);

// Cancel the head of a long FIFO queue.
static void BM_Cancel_Head(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    std::vector<OrderId> ids;
    ids.reserve(N);
    for (auto _ : state) {
        state.PauseTiming();
        EngineFixture f;
        for (int i = 0; i < N; ++i) {
            f.engine.submit_limit(InstrumentId{1},
                                  TraderId{static_cast<std::uint64_t>(i)+1},
                                  Side::Buy, Price{100}, Quantity{1},
                                  TimeInForce::GTC, f.sink);
            ids.push_back(f.sink.results.back().order_id);
        }
        state.ResumeTiming();
        benchmark::DoNotOptimize(
            f.engine.cancel(InstrumentId{1}, ids.front()));
        ids.clear();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetComplexityN(N);
}
BENCHMARK(BM_Cancel_Head)
    ->RangeMultiplier(4)->Range(1<<4, 1<<12)
    ->Unit(benchmark::kMicrosecond);

// Submit + immediate cancel — simulates a fast-spam trader.
static void BM_SubmitThenCancel(benchmark::State& state) {
    EngineFixture f;
    int64_t counter = 0;
    for (auto _ : state) {
        f.engine.submit_limit(InstrumentId{1}, TraderId{1}, Side::Buy,
                              Price{100}, Quantity{10}, TimeInForce::GTC, f.sink);
        OrderId id = f.sink.results.back().order_id;
        benchmark::DoNotOptimize(f.engine.cancel(InstrumentId{1}, id));
        ++counter;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SubmitThenCancel);

}  // namespace

BENCHMARK_MAIN();