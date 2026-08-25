// include/tt/concurrency/worker_pool.hpp
//
// Per-instrument worker pool. Each instrument is sharded to one worker
// thread, which owns a `MatchingEngine` and processes inbound orders
// through an SPSC queue.
//
// The gateway thread (or any producer) calls `submit(instrument, order)`
// which enqueues the request into the right worker's inbox. The worker
// thread drains the inbox, runs the order through the matching engine,
// and writes outbound events to an outbound SPSC queue that the gateway
// drains.
//
// Concurrency model:
//   - One MatchingEngine per worker.
//   - One inbound queue per worker (SPSC).
//   - One outbound queue shared between workers and the gateway (MPSC
//     drained by a single consumer — the gateway).
//
// Threading: MPSC outbound is implemented via a small lock-free multi-
// producer queue (MPSCRingQueue) below. Inbound is SPSC per worker.

#pragma once

#include "tt/common/types.hpp"
#include "tt/concurrency/spsc_queue.hpp"
#include "tt/core/order.hpp"
#include "tt/market_data/publisher.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/persistence/event_log.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tt {

// ---- Outbound event --------------------------------------------------------
enum class OutboundKind : uint8_t {
    Trade      = 0,
    Submit     = 1,
    TopOfBook  = 2,
    Error      = 3,
};

#pragma pack(push, 1)
struct SubmitResultLite {
    SubmitStatus status{SubmitStatus::Accepted};
    char         pad[2]{};
    OrderId      order_id{kInvalidOrderId};
    InstrumentId instrument_id{kInvalidInstrumentId};
    Sequence     sequence{0};
    Quantity     filled_quantity{Quantity{0}};
    Quantity     resting_quantity{Quantity{0}};
    Side         side{Side::Buy};
    OrderType    type{OrderType::Limit};
    Price        price{Price{0}};
    char         reject_reason[128]{};
};

struct OutboundEvent {
    OutboundKind    kind{OutboundKind::Submit};
    char            pad0[7]{};
    Trade           trade{};
    SubmitResultLite result{};
    TopOfBookSnapshot tob{};
    char            error_message[128]{};
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<OutboundEvent>,
              "OutboundEvent must be trivially copyable.");

// ---- MPSC queue for outbound ----------------------------------------------
// Each worker is a producer; the gateway (or test thread) is the consumer.
// Lock-free would be ideal but a tiny mutex is fine for the project.
// In production this would be replaced by a proper MPSC ring (e.g. the
// Dmitry Vyukov N-producers / 1-consumer pattern).
class MPSCRingQueue {
public:
    bool push(const OutboundEvent& v) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (buf_.size() >= capacity_) return false;
        buf_.push_back(v);
        return true;
    }

    bool pop(OutboundEvent& v) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (buf_.empty()) return false;
        v = buf_.front();
        buf_.erase(buf_.begin());
        return true;
    }

private:
    static constexpr std::size_t capacity_ = 65536;
    std::mutex                      mtx_;
    std::vector<OutboundEvent>      buf_;
};

// ---- Inbound submission ---------------------------------------------------
// Inbound is a raw Order pointer. Ownership transfers from producer to
// worker thread (the worker destroys it on completion).
struct InboundSubmission {
    Order* order{nullptr};
};
static_assert(std::is_trivially_copyable_v<InboundSubmission>,
              "InboundSubmission must be trivially copyable.");

// ---- Worker ----------------------------------------------------------------
class Worker {
public:
    Worker(InstrumentId instrument, MPSCRingQueue* outbound) noexcept;
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();
    void stop();

    bool submit(std::unique_ptr<Order> o);

    MatchingEngine& engine() noexcept { return *engine_; }

private:
    void run();

    InstrumentId                              instrument_;
    MatchingEngine*                           engine_{nullptr};   // owned
    MarketDataPublisher*                      publisher_{nullptr};
    SPSCQueue<InboundSubmission, 4096>        inbox_;
    MPSCRingQueue*                    outbound_{nullptr};
    std::thread                               thread_;
    std::atomic<bool>                         running_{false};
};

// ---- WorkerPool ------------------------------------------------------------
class WorkerPool {
public:
    explicit WorkerPool(MPSCRingQueue* outbound) : outbound_(outbound) {}
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Create a worker for `instrument` if it doesn't already exist.
    Worker& get_or_create(InstrumentId instrument);

    // Submit an order to the worker for `instrument`. Returns false if no
    // worker is registered or the inbox is full.
    bool submit(InstrumentId instrument, std::unique_ptr<Order> order);

    // Stop all workers.
    void shutdown();

    std::size_t worker_count() const noexcept { return workers_.size(); }

private:
    MPSCRingQueue* outbound_;
    mutable std::mutex    mtx_;
    std::unordered_map<InstrumentId, std::unique_ptr<Worker>> workers_;
};

}  // namespace tt
