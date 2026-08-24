// include/tt/matching/matching_engine.hpp
//
// Deterministic single-threaded matching engine.
//
// Algorithm (price-time priority, per instrument):
//   1. accept incoming order; assign sequence number; classify as limit/market.
//   2. WHILE (incoming.qty > 0 AND opposite.book not empty AND price match):
//        a. look at best opposite price.
//        b. for limit: best_ask <= incoming.price (buy) OR best_bid >=
//           incoming.price (sell). If not, break.
//        c. fill incoming against best_opposite front-of-queue up to
//           min(incoming.remaining, opposite.front.remaining).
//        d. emit Trade via sink; decrement remaining on both sides.
//        e. if opposite.front now zero, drop it and advance to next level.
//   3. IF incoming still has remaining:
//        - limit GTC: insert into book (rests).
//        - limit IOC: drop remainder, emit nothing further.
//        - market  : drop remainder (no resting).
//
// Determinism:
//   - Sequence numbers are monotonic and assigned at submission time.
//   - The engine processes submissions strictly in the order received when
//     driven from a single thread. The matcher does no allocation in the
//     hot path beyond Order ownership transfer (the Order* is moved into
//     the book on rest).
//
// Concurrency:
//   - This class is NOT thread-safe. Phase 9 wraps it with a per-instrument
//     mutex or assigns one engine per worker thread.

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/orderbook/order_book.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tt {

// What the engine tells the outside world about a single submission.
enum class SubmitStatus : uint8_t {
    Accepted        = 0,  // resting in the book (full or partial)
    FullyFilled     = 1,  // completely filled by liquidity
    PartiallyFilled = 2,  // some fills then remainder cancelled (IOC / market)
    Rejected        = 3,  // invalid input, or no liquidity for market order
    Cancelled       = 4,  // user-initiated cancel (Phase 3)
};

// Event emitted to a TradeSink.
struct SubmitResult {
    SubmitStatus      status{SubmitStatus::Accepted};
    OrderId           order_id{kInvalidOrderId};
    Sequence          sequence{0};
    Quantity          filled_quantity{Quantity{0}};   // sum of fills
    Quantity          resting_quantity{Quantity{0}};  // what's left in the book
    std::string       reject_reason;                  // empty on success
};

// Sink callback for trades and final accept/reject. Called inline from the
// matching thread so the consumer must not block.
class TradeSink {
public:
    virtual ~TradeSink() = default;
    // Called once per matched fill.
    virtual void on_trade(const Trade& t) noexcept = 0;
    // Called once after the matcher has finished processing the submission.
    virtual void on_submit_result(const SubmitResult& r) noexcept = 0;
};

// Concrete trade sink that just collects trades in a vector — useful for
// tests and benchmarks.
class CollectingSink final : public TradeSink {
public:
    void on_trade(const Trade& t) noexcept override {
        trades.push_back(t);
    }
    void on_submit_result(const SubmitResult& r) noexcept override {
        results.push_back(r);
    }

    std::vector<Trade>        trades;
    std::vector<SubmitResult> results;
};

class MatchingEngine {
public:
    MatchingEngine();
    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&)            = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Register an instrument. Idempotent. The caller chooses InstrumentIds.
    void register_instrument(InstrumentId id);

    // Drop everything. Test helper.
    void clear();

    // Submit an order. The engine takes ownership of `order` (it will be
    // destroyed when fully filled, cancelled or the engine itself dies).
    // The submission is processed synchronously on the calling thread.
    void submit(std::unique_ptr<Order> order, TradeSink& sink);

    // Convenience for tests/benchmarks: build a limit order and submit it.
    void submit_limit(InstrumentId instrument, TraderId trader, Side side,
                      Price price, Quantity qty, TimeInForce tif,
                      TradeSink& sink);

    // Cancel a resting order. Returns true if cancelled.
    bool cancel(InstrumentId instrument, OrderId id);

    // Read-only access to a book for inspection (tests, market data).
    OrderBook*       book(InstrumentId id)       noexcept;
    const OrderBook* book(InstrumentId id) const noexcept;

    // Sequence / metrics.
    Sequence next_sequence() const noexcept { return next_sequence_; }
    std::size_t instrument_count() const noexcept { return books_.size(); }

private:
    // Drive the match loop. Returns true if there's still resting qty.
    bool match_against(Order& taker, TradeSink& sink);

    // Insert the taker (still has remaining qty) into the book as a rest.
    void rest(Order& taker);

    // Helpers
    static bool crosses(Side aggressor, Price taker_price, Price book_price) noexcept {
        // Buy: book_price <= taker_price. Sell: book_price >= taker_price.
        return aggressor == Side::Buy ? book_price <= taker_price
                                      : book_price >= taker_price;
    }

    std::unordered_map<InstrumentId, std::unique_ptr<OrderBook>> books_;
    Sequence next_sequence_{1};
};

}  // namespace tt
