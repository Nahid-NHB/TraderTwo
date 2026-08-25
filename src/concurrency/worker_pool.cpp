// src/concurrency/worker_pool.cpp

#include "tt/concurrency/worker_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace tt {

namespace {
inline void copy_error(char (&dst)[128], const std::string& src) {
    std::size_t n = std::min(src.size(), sizeof(dst) - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

inline SubmitResultLite to_lite(const SubmitResult& r) {
    SubmitResultLite out{};
    out.status          = r.status;
    out.order_id        = r.order_id;
    out.instrument_id   = r.instrument_id;
    out.sequence        = r.sequence;
    out.filled_quantity = r.filled_quantity;
    out.resting_quantity= r.resting_quantity;
    out.side            = r.side;
    out.type            = r.type;
    out.price           = r.price;
    copy_error(out.reject_reason, r.reject_reason);
    return out;
}
}  // namespace

// ---- Worker ----------------------------------------------------------------
class WorkerSink final : public TradeSink {
public:
    WorkerSink(InstrumentId id, MPSCRingQueue* out) noexcept
        : instrument_(id), outbound_(out) {}
    void on_trade(const Trade& t) noexcept override {
        OutboundEvent e{};
        e.kind = OutboundKind::Trade;
        e.trade = t;
        outbound_->push(e);
    }
    void on_submit_result(const SubmitResult& r) noexcept override {
        OutboundEvent e{};
        e.kind = OutboundKind::Submit;
        e.result = to_lite(r);
        copy_error(e.error_message, r.reject_reason);
        outbound_->push(e);
    }
private:
    InstrumentId instrument_;
    MPSCRingQueue* outbound_;
};

Worker::Worker(InstrumentId instrument, MPSCRingQueue* outbound) noexcept
    : instrument_(instrument), outbound_(outbound) {}

Worker::~Worker() {
    stop();
}

void Worker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    engine_    = new MatchingEngine();
    publisher_ = new MarketDataPublisher(*engine_);
    engine_->register_instrument(instrument_);
    thread_ = std::thread([this]{ run(); });
}

void Worker::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    // Push a poison-pill (raw null pointer). The worker exits when it pops
    // a record with order == nullptr.
    InboundSubmission poison{};
    poison.order = nullptr;
    inbox_.push(poison);
    if (thread_.joinable()) thread_.join();
    delete publisher_;
    delete engine_;
    publisher_ = nullptr;
    engine_    = nullptr;
}

bool Worker::submit(std::unique_ptr<Order> o) {
    InboundSubmission in;
    in.order = o.release();   // ownership transfers to the worker
    return inbox_.push(in);
}

void Worker::run() {
    WorkerSink sink(instrument_, outbound_);
    while (running_.load(std::memory_order_acquire)) {
        InboundSubmission in;
        if (!inbox_.pop(in)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        if (!in.order) break;  // poison
        std::unique_ptr<Order> guard(in.order);
        engine_->submit(std::move(guard), sink);
    }
}

// ---- WorkerPool ------------------------------------------------------------
WorkerPool::~WorkerPool() { shutdown(); }

Worker& WorkerPool::get_or_create(InstrumentId instrument) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = workers_.find(instrument);
    if (it != workers_.end()) return *it->second;
    auto w = std::make_unique<Worker>(instrument, outbound_);
    w->start();
    Worker& ref = *w;
    workers_.emplace(instrument, std::move(w));
    return ref;
}

bool WorkerPool::submit(InstrumentId instrument, std::unique_ptr<Order> order) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = workers_.find(instrument);
    if (it == workers_.end()) return false;
    return it->second->submit(std::move(order));
}

void WorkerPool::shutdown() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [_, w] : workers_) {
        (void)_;
        w->stop();
    }
    workers_.clear();
}

}  // namespace tt