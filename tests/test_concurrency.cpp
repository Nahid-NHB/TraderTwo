// tests/test_concurrency.cpp
//
// Tests for SPSC queue and worker pool.

#include "tt/common/types.hpp"
#include "tt/concurrency/spsc_queue.hpp"
#include "tt/concurrency/worker_pool.hpp"
#include "tt/core/order.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace tt;

namespace {
Price p(int64_t v) { return Price{v}; }
Quantity q(int64_t v) { return Quantity{v}; }

std::unique_ptr<Order> make_order(TraderId trader, InstrumentId instr,
                                  Side side, Price price, Quantity qty) {
    auto o = std::make_unique<Order>();
    o->trader_id     = trader;
    o->instrument_id = instr;
    o->side          = side;
    o->type          = OrderType::Limit;
    o->tif           = TimeInForce::GTC;
    o->price         = price;
    o->quantity      = qty;
    o->remaining     = qty;
    o->status        = OrderStatus::New;
    return o;
}
}  // namespace

// ---------------------------------------------------------------------------
// SPSCQueue
// ---------------------------------------------------------------------------
TEST(SPSCQueue, BasicRoundTrip) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));

    int v = 0;
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);
    EXPECT_TRUE(q.pop(v)); EXPECT_EQ(v, 3);
    EXPECT_FALSE(q.pop(v));
}

TEST(SPSCQueue, FullQueueReturnsFalse) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    // Capacity is 4 — we should be able to push 4 items then fail on the 5th.
    EXPECT_TRUE(q.push(4));
    EXPECT_FALSE(q.push(5));
    int v; EXPECT_TRUE(q.pop(v));
}

TEST(SPSCQueue, SPSCUnderLoad) {
    SPSCQueue<int, 1024> q;
    constexpr int N = 1000;

    std::thread producer([&]{
        for (int i = 0; i < N; ++i) {
            while (!q.push(i)) std::this_thread::yield();
        }
    });

    int received = 0;
    int last = -1;
    std::thread consumer([&]{
        while (received < N) {
            int v;
            if (q.pop(v)) {
                EXPECT_GE(v, last);
                last = v;
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();
    EXPECT_EQ(received, N);
}

// ---------------------------------------------------------------------------
// WorkerPool
// ---------------------------------------------------------------------------
TEST(WorkerPool, SubmitsAcrossInstruments) {
    MPSCRingQueue out;
    WorkerPool pool(&out);

    pool.get_or_create(/*instr=*/1);
    pool.get_or_create(/*instr=*/2);
    EXPECT_EQ(pool.worker_count(), 2u);

    // Submit two orders, one per instrument.
    EXPECT_TRUE(pool.submit(1, make_order(10, 1, Side::Sell, p(100), q(50))));
    EXPECT_TRUE(pool.submit(2, make_order(11, 2, Side::Buy,  p(200), q(30))));

    // Give workers a moment to drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int submits = 0;
    OutboundEvent ev;
    while (out.pop(ev)) {
        if (ev.kind == OutboundKind::Submit) ++submits;
    }
    EXPECT_EQ(submits, 2);

    pool.shutdown();
}

TEST(WorkerPool, UnknownInstrumentReturnsFalse) {
    MPSCRingQueue out;
    WorkerPool pool(&out);
    EXPECT_FALSE(pool.submit(99, make_order(1, 99, Side::Buy, p(100), q(10))));
    pool.shutdown();
}