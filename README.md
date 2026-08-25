# TraderTwo

A deterministic, single-threaded-per-instrument **C++20 matching engine** for limit-order books, built end-to-end across 14 phases: price-time priority order book, multi-instrument matching, cancel/modify, market-data publisher, append-only event log with replay-based crash recovery, pre-trade risk checks, per-instrument worker pool, line-oriented TCP gateway, market simulator, Google-Benchmark scenarios, Prometheus/JSON observability, an RFC 6455 WebSocket market-data endpoint, and a SvelteKit dashboard SPA.

> Educational reference implementation. Not production-grade — the matcher is single-threaded, the gateway is BSD-socket based, and the persistence layer has no CRC.

---

## Highlights

| Metric | Value |
|---|---|
| Unit tests | **125** across 20 suites, all passing |
| Sustained throughput | ~5M submit+cancel/s · ~5M orders/s simulator · ~1.8M same-level inserts/s |
| Stress test | 200K mixed orders across 8 instruments in ~0.6 s |
| Standards | C++20, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion` |
| Build | CMake 3.20+, GCC/Clang, pthreads, OpenSSL, Node 18+ (for dashboard) |

---

## Quick start

```sh
# Configure & build (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run all tests
ctest --test-dir build --output-on-failure
# 100% tests passed out of 125

# Run the smoke demo
./build/tt_demo
```

### Run the simulator

Generate 10K random orders across 4 instruments and write trades to an event log:

```sh
./build/tt_simulator --random 10000 --instruments 4 --log run.bin
```

Replay the log back into a fresh engine:

```sh
# (see tests/test_event_log.cpp for the replay pattern; a CLI replay tool
# is left as an exercise.)
```

Replay a script file:

```sh
cat > orders.txt <<'EOF'
PING
NEW 1 1 1 100 10 GTC
NEW 1 2 0 100 5 GTC
QUOTE 1
EOF

./build/tt_simulator --script orders.txt
```

### Run the TCP gateway

```sh
# Start the gateway on 127.0.0.1:9000
./build/tt_simulator --random 1000 --port 9000

# From another terminal, drive it with raw TCP
{ echo PING; echo 'NEW 1 1 0 100 10 GTC'; echo 'QUOTE 1'; } | nc 127.0.0.1 9000
```

Protocol grammar (one command per line):

```
PING
NEW   <instrument> <trader> <side 0|1> <price> <qty> [GTC|IOC|FOK]
CANCEL <instrument> <order_id>
MODIFY <instrument> <order_id> <new_qty> <new_price>
QUOTE <instrument>
```

Replies:

```
PONG
OK <ACCEPTED|FILLED|PARTIAL|CANCELLED|REJECTED> <order_id> <sequence>
TRADE <buy_id> <sell_id> <price> <qty>
QUOTE <instrument> <bid> <bid_qty> <ask> <ask_qty>
ERR <message>
```

### Run benchmarks

```sh
./build/tt_benchmarks --benchmark_min_time=0.5s
```

Six scenarios covering insertion, full matching, single-level queue growth, modify-reduce, cancel-head, and submit-then-cancel.

### Run the live dashboard

```sh
# Terminal 1 — start the engine with both TCP and WS endpoints
./build/tt_simulator --random 50000 --instruments 4 \
                     --port 9000 --ws-port 9001

# Terminal 2 — build & run the SvelteKit dashboard
cd dashboard
npm install
npm run dev
# open http://127.0.0.1:5173/?port=9001
```

The C++ side exposes a minimal RFC 6455 WebSocket server (`src/networking/ws_server.cpp`) that broadcasts JSON frames for every trade and top-of-book update. The dashboard (under `dashboard/`) is a SvelteKit SPA styled like TradingView: candlestick chart (powered by TradingView Lightweight Charts™), depth visualisation, order-book ladder, and time-and-sales tape.

Message format:

```
{"type":"trade","i":1,"buy":2,"sell":1,"px":100,"qty":3,"seq":2}
{"type":"tob","i":1,"b":9600,"bq":24,"a":9900,"aq":2,"hb":1,"ha":1}
```

---

## Repository layout

```
include/tt/
  common/        types & instrument descriptor
  core/          Order, Trade
  orderbook/     price-time priority book (intrusive FIFO)
  matching/      deterministic single-threaded matcher
  market_data/   Event, publisher, recorder
  persistence/   append-only event log + replayer
  risk/          pre-trade risk gate (max qty, max notional, price collar, rate limit)
  concurrency/   SPSC queue + per-instrument worker pool
  networking/    line-oriented protocol + BSD-socket gateway
  observability/ metrics (Prometheus + JSON) + structured JSON logger

src/              implementations of the above
examples/         demo_phase1.cpp, simulator.cpp
benchmarks/       bench_phase1.cpp (Google-Benchmark)
dashboard/        SvelteKit SPA for live market-data visualisation
tests/            125 unit + stress tests
notes.md          14-phase build plan
```

---

## License

Personal learning project — use at your own risk.