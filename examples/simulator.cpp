// examples/simulator.cpp
//
// Phase 10: Market simulator CLI.
//
// Two modes:
//   --script <file>   Read newline-delimited commands (NEW/CANCEL/MODIFY/QUOTE)
//                     from <file> and feed them through the matching engine.
//   --random <N>      Generate <N> synthetic orders across <M> instruments
//                     using a uniform-random walk, then submit them all.
//
// Output: each accepted order and each trade is printed to stdout. With
// --log <path>, every event is also persisted to an append-only event log
// for replay.
//
// Flags:
//   --instruments M   Number of instruments in random mode (default: 4).
//   --seed S          PRNG seed for random mode (default: 42).
//   --log <path>      Path to event log (optional).
//   --script <path>   Read commands from this file (one per line).
//   --random <N>      Generate N random orders.
//   --port P          If set, also start a TCP gateway on port P (in addition
//                     to running the simulation synchronously on the main
//                     thread first).
//   --ws-port P       If set, also start a WebSocket market-data server on
//                     port P after the sim completes. Use this with the
//                     dashboard SPA under dashboard/.
//   --quiet           Don't print per-trade / per-order lines; only summary.
//
// Examples:
//   ./tt_simulator --script orders.txt --log run.bin
//   ./tt_simulator --random 100000 --instruments 8 --log run.bin
//   ./tt_simulator --random 1000 --port 9000
//   ./tt_simulator --random 100000 --instruments 4 --port 9000 --ws-port 9001

#include "tt/common/instrument.hpp"
#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/market_data/publisher.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/networking/gateway.hpp"
#include "tt/networking/protocol.hpp"
#include "tt/networking/ws_server.hpp"
#include "tt/persistence/event_log.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace tt;

namespace {

std::atomic<bool> g_done{false};

extern "C" void on_signal(int) { g_done.store(true); }

// ------------------------------------------------------------------
// StdoutTradeSink — prints trades/accepts to the console (or quiet).
// ------------------------------------------------------------------
class StdoutTradeSink final : public TradeSink {
public:
    explicit StdoutTradeSink(bool quiet, TradeSink* downstream)
        : quiet_(quiet), downstream_(downstream) {}
    void on_trade(const Trade& t) noexcept override {
        ++trade_count_;
        if (!quiet_) {
            std::printf("TRADE inst=%lu buy=%lu sell=%lu px=%lld qty=%lld seq=%lu\n",
                        static_cast<unsigned long>(t.instrument_id),
                        static_cast<unsigned long>(t.buy_order_id),
                        static_cast<unsigned long>(t.sell_order_id),
                        static_cast<long long>(t.price.ticks),
                        static_cast<long long>(t.quantity.qty),
                        static_cast<unsigned long>(t.sequence));
        }
        if (downstream_) downstream_->on_trade(t);
    }
    void on_submit_result(const SubmitResult& r) noexcept override {
        ++submit_count_;
        if (!quiet_) {
            const char* s = "ACCEPTED";
            switch (r.status) {
                case SubmitStatus::Accepted:        s = "ACCEPTED";   break;
                case SubmitStatus::FullyFilled:     s = "FILLED";     break;
                case SubmitStatus::PartiallyFilled: s = "PARTIAL";    break;
                case SubmitStatus::Cancelled:       s = "CANCELLED";  break;
                case SubmitStatus::Rejected:        s = "REJECTED";   break;
            }
            if (r.status == SubmitStatus::Rejected) {
                std::printf("OK %s inst=%lu id=%lu seq=%lu reason=\"%s\"\n",
                            s,
                            static_cast<unsigned long>(r.instrument_id),
                            static_cast<unsigned long>(r.order_id),
                            static_cast<unsigned long>(r.sequence),
                            r.reject_reason.c_str());
            } else {
                std::printf("OK %s inst=%lu id=%lu seq=%lu\n",
                            s,
                            static_cast<unsigned long>(r.instrument_id),
                            static_cast<unsigned long>(r.order_id),
                            static_cast<unsigned long>(r.sequence));
            }
        }
        if (downstream_) downstream_->on_submit_result(r);
    }
    std::size_t trade_count()  const noexcept { return trade_count_; }
    std::size_t submit_count() const noexcept { return submit_count_; }
private:
    bool       quiet_;
    TradeSink* downstream_;
    std::size_t trade_count_{0};
    std::size_t submit_count_{0};
};

// ------------------------------------------------------------------
// Scripted mode — parse and submit lines like:
//   NEW 1 100 0 100 10 GTC
//   CANCEL 1 5
//   MODIFY 1 5 5 100
//   QUOTE 1
//   PING
// ------------------------------------------------------------------
bool run_script(const std::string& path, MatchingEngine& engine,
                TradeSink& sink) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "failed to open script file: %s\n", path.c_str());
        return false;
    }

    std::string line;
    std::size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        // Strip trailing \r.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        ParsedRequest req;
        auto err = parse_request(line, req);
        if (!err.ok) {
            std::fprintf(stderr, "line %zu: parse error: %s\n", lineno, err.message.c_str());
            continue;
        }

        switch (req.command) {
            case InboundCommand::Ping:
                std::printf("PONG\n");
                break;
            case InboundCommand::Quote: {
                OrderBook* ob = engine.book(req.instrument);
                if (!ob) { std::fprintf(stderr, "line %zu: instrument %lu not registered\n",
                                       lineno, static_cast<unsigned long>(req.instrument)); break; }
                auto b = ob->top_bid();
                auto a = ob->top_ask();
                std::printf("QUOTE inst=%lu bid=%lld bid_qty=%lld ask=%lld ask_qty=%lld\n",
                            static_cast<unsigned long>(req.instrument),
                            b.valid ? static_cast<long long>(b.price.ticks) : -1,
                            b.valid ? static_cast<long long>(b.quantity.qty) : 0,
                            a.valid ? static_cast<long long>(a.price.ticks) : -1,
                            a.valid ? static_cast<long long>(a.quantity.qty) : 0);
                break;
            }
            case InboundCommand::New: {
                engine.submit_limit(req.instrument, req.trader_id, req.side,
                                    req.price, req.qty, req.tif, sink);
                break;
            }
            case InboundCommand::Cancel:
                if (!engine.cancel(req.instrument, req.order_id)) {
                    std::fprintf(stderr, "line %zu: cancel id=%lu failed\n",
                                 lineno, static_cast<unsigned long>(req.order_id));
                }
                break;
            case InboundCommand::Modify: {
                auto r = engine.modify(req.instrument, req.order_id, req.qty, req.price);
                if (r == OrderBook::ModifyResult::NotFound) {
                    std::fprintf(stderr, "line %zu: modify id=%lu not found\n",
                                 lineno, static_cast<unsigned long>(req.order_id));
                }
                break;
            }
            default:
                std::fprintf(stderr, "line %zu: unknown command\n", lineno);
                break;
        }
    }
    return true;
}

// ------------------------------------------------------------------
// Random mode — generate synthetic liquidity around an anchor price.
// ------------------------------------------------------------------
struct RandomConfig {
    int instruments{4};
    std::uint64_t seed{42};
    int orders{0};
};

void run_random(const RandomConfig& cfg, MatchingEngine& engine,
                TradeSink& sink) {
    std::mt19937_64 rng(cfg.seed);
    // Uniform distribution over a wide price range around the anchor.
    std::uniform_int_distribution<int> price_dist(95, 105);
    std::uniform_int_distribution<int> qty_dist(1, 50);
    std::uniform_int_distribution<int> side_dist(0, 1);

    const int M = std::max(1, cfg.instruments);
    const int N = std::max(0, cfg.orders);

    for (int i = 0; i < N; ++i) {
        InstrumentId instr = InstrumentId{static_cast<std::uint64_t>((i % M) + 1)};
        Side         side  = (side_dist(rng) == 0) ? Side::Buy : Side::Sell;
        Price        price = Price{price_dist(rng) * 100};
        Quantity     qty   = Quantity{qty_dist(rng)};
        TraderId     trader = TraderId{static_cast<std::uint64_t>(rng() % 100) + 1};
        engine.submit_limit(instr, trader, side, price, qty, TimeInForce::GTC, sink);
    }
}

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [--script FILE] [--random N] [--instruments M]\n"
        "          [--seed S] [--log FILE] [--port P] [--quiet]\n"
        "\n"
        "  --script FILE    Read commands (NEW/CANCEL/MODIFY/QUOTE/PING) from FILE.\n"
        "  --random N       Generate N synthetic orders.\n"
        "  --instruments M  Number of instruments (random mode). Default: 4.\n"
        "  --seed S         PRNG seed (random mode). Default: 42.\n"
        "  --log FILE       Persist events to this event log.\n"
        "  --port P         Also start TCP gateway on port P after sim completes.\n"
        "  --ws-port P      Also start WebSocket market-data server on port P.\n"
        "  --quiet          Don't print per-event lines; only summary.\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    std::string script_path;
    std::string log_path;
    bool        random_mode = false;
    bool        quiet       = false;
    int         port        = 0;
    int         ws_port     = 0;
    RandomConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if      (a == "--script")     script_path = need("--script");
        else if (a == "--random")     { random_mode = true; cfg.orders = std::atoi(need("--random")); }
        else if (a == "--instruments") cfg.instruments = std::atoi(need("--instruments"));
        else if (a == "--seed")        cfg.seed = std::stoull(need("--seed"));
        else if (a == "--log")         log_path  = need("--log");
        else if (a == "--port")        port      = std::atoi(need("--port"));
        else if (a == "--ws-port")     ws_port   = std::atoi(need("--ws-port"));
        else if (a == "--quiet")       quiet     = true;
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown flag: %s\n", a.c_str()); print_usage(argv[0]); return 2; }
    }

    if (!random_mode && script_path.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    // Register instruments.
    MatchingEngine engine;
    const int M = std::max(1, cfg.instruments);
    for (int i = 1; i <= M; ++i) {
        InstrumentDescriptor d{};
        d.id          = InstrumentId{static_cast<std::uint64_t>(i)};
        d.symbol      = "INST" + std::to_string(i);
        d.display_name= d.symbol;
        d.tick_size   = Price{1};
        d.lot_size    = Quantity{1};
        d.enabled     = true;
        engine.register_instrument(d);
    }

    // Wire sinks: stdout → optionally persisted.
    std::unique_ptr<EventLog> log;
    std::unique_ptr<PersistingSink> persisting;
    if (!log_path.empty()) {
        log = std::make_unique<EventLog>(log_path, /*append=*/false);
        persisting = std::make_unique<PersistingSink>(*log);
    }
    StdoutTradeSink sink(quiet, persisting.get());

    auto t0 = std::chrono::steady_clock::now();

    if (random_mode) {
        run_random(cfg, engine, sink);
    } else {
        if (!run_script(script_path, engine, sink)) return 1;
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();

    if (log) log->close();

    std::printf("\n=== Summary ===\n");
    std::printf("submits      = %zu\n", sink.submit_count());
    std::printf("trades       = %zu\n", sink.trade_count());
    std::printf("instruments  = %d\n", M);
    std::printf("next_seq     = %lu\n",
                static_cast<unsigned long>(engine.next_sequence()));
    std::printf("elapsed      = %.3f s\n", elapsed_s);
    if (elapsed_s > 0.0 && cfg.orders > 0) {
        std::printf("throughput   = %.0f orders/s\n",
                    static_cast<double>(cfg.orders) / elapsed_s);
    }
    if (!log_path.empty()) {
        std::printf("event log    = %s\n", log_path.c_str());
    }

    if (port > 0 || ws_port > 0) {
        MarketDataPublisher pub(engine);
        std::unique_ptr<Gateway>   gw;
        std::unique_ptr<WsServer>  ws;
        if (port > 0) {
            gw = std::make_unique<Gateway>(engine, pub, static_cast<std::uint16_t>(port));
            if (!gw->start()) {
                std::fprintf(stderr, "failed to start gateway on port %d\n", port);
                return 1;
            }
            std::printf("gateway listening on 127.0.0.1:%d\n", port);
        }
        if (ws_port > 0) {
            ws = std::make_unique<WsServer>(pub, static_cast<std::uint16_t>(ws_port));
            if (!ws->start()) {
                std::fprintf(stderr, "failed to start ws server on port %d\n", ws_port);
                if (gw) gw->stop();
                return 1;
            }
            std::printf("websocket listening on 127.0.0.1:%d\n", ws_port);
        }

        // Wait for Ctrl-C.
        std::signal(SIGINT,  on_signal);
        std::signal(SIGTERM, on_signal);
        while (!g_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (ws) { ws->stop(); std::printf("websocket stopped\n"); }
        if (gw) { gw->stop(); std::printf("gateway stopped\n"); }
    }
    return 0;
}