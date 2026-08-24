// examples/demo_phase1.cpp
//
// Tiny smoke program that exercises the Phase 1 order book. It is not a
// benchmark, just a runnable demonstration.

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/orderbook/order_book.hpp"

#include <cstdio>
#include <memory>

using namespace tt;

namespace {
std::unique_ptr<Order> make_limit(OrderId id, Side side, Price p, Quantity q,
                                   Sequence seq) {
    auto o = std::make_unique<Order>();
    o->id            = id;
    o->trader_id     = 1;
    o->instrument_id = 42;
    o->side          = side;
    o->type          = OrderType::Limit;
    o->tif           = TimeInForce::GTC;
    o->price         = p;
    o->quantity      = q;
    o->remaining     = q;
    o->sequence      = seq;
    o->status        = OrderStatus::New;
    return o;
}
}  // namespace

int main() {
    OrderBook book(/*instrument=*/42);

    book.insert(make_limit(1, Side::Buy,  Price{100'000'000}, Quantity{100}, 1));
    book.insert(make_limit(2, Side::Buy,  Price{100'000'000}, Quantity{200}, 2));
    book.insert(make_limit(3, Side::Buy,  Price{ 99'500'000}, Quantity{150}, 3));
    book.insert(make_limit(4, Side::Sell, Price{100'500'000}, Quantity{100}, 4));
    book.insert(make_limit(5, Side::Sell, Price{100'500'000}, Quantity{ 50}, 5));

    std::printf("best bid = %lld  qty = %lld  levels = %zu\n",
                static_cast<long long>(book.best_bid().ticks),
                static_cast<long long>(book.best_bid_quantity().qty),
                book.bid_level_count());
    std::printf("best ask = %lld  qty = %lld  levels = %zu\n",
                static_cast<long long>(book.best_ask().ticks),
                static_cast<long long>(book.best_ask_quantity().qty),
                book.ask_level_count());
    std::printf("total orders resting = %zu\n", book.total_order_count());

    bool cancelled = book.cancel(/*id=*/2);
    std::printf("cancel id=2 -> %s   best bid qty now = %lld\n",
                cancelled ? "ok" : "fail",
                static_cast<long long>(book.best_bid_quantity().qty));

    return 0;
}
