# TraderTwo

A deterministic, single-threaded-per-instrument **C++20 matching engine** for limit-order books, built end-to-end across 15 phases: price-time priority order book, multi-instrument matching, cancel/modify, market-data publisher, append-only event log with replay-based crash recovery, pre-trade risk checks, per-instrument worker pool, line-oriented TCP gateway, market simulator, Google-Benchmark scenarios, Prometheus/JSON observability, an RFC 6455 WebSocket market-data endpoint with typed command protocol, a SvelteKit dashboard SPA, and a multi-instrument tabbed layout with order-entry panel.

> Educational reference implementation. Not production-grade — the matcher is single-threaded, the gateway is BSD-socket based, and the persistence layer has no CRC.

---

## Highlights

| Metric | Value |
|---|---|
| Unit tests | **132** across 20 suites, all passing |
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
# 100% tests passed out of 132

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
# Terminal 1 — start the engine with both TCP and WS endpoints.
# The WS endpoint also accepts client commands (submit/cancel/modify/ping).
./build/tt_simulator --random 50000 --instruments 4 \
                     --port 9000 --ws-port 9001

# Terminal 2 — build & run the SvelteKit dashboard
cd dashboard
npm install
npm run dev
# open http://127.0.0.1:5173/?port=9001
```

The C++ side exposes a minimal RFC 6455 WebSocket server (`src/networking/ws_server.cpp`) that broadcasts JSON frames for every trade and top-of-book update. The dashboard (under `dashboard/`) is a SvelteKit SPA styled like TradingView: candlestick chart (powered by TradingView Lightweight Charts™), depth visualisation, order-book ladder, time-and-sales tape, **multi-instrument tabbed layout with a watch grid showing every instrument's BBO side-by-side**, and an **order-entry panel** that sends typed commands back to the engine over the same WebSocket connection.

Server-to-client broadcast frames:

```
{"type":"trade","i":1,"buy":2,"sell":1,"px":100,"qty":3,"seq":2}
{"type":"tob","i":1,"b":9600,"bq":24,"a":9900,"aq":2,"hb":1,"ha":1}
```

Client-to-server commands (sent as JSON text frames; `req` is an opaque
echo tag round-tripped in the reply):

```
{"type":"ping","req":"abc"}
{"type":"submit","req":"r1","i":1,"trader":99,"side":0,"px":100,"qty":5,"tif":"GTC"}
{"type":"cancel","req":"r2","i":1,"id":7}
{"type":"modify","req":"r3","i":1,"id":7,"qty":3,"px":101}
```

Replies:

```
{"type":"pong","req":"abc"}
{"type":"submit_result","req":"r1","status":"ACCEPTED","id":7,"i":1,"seq":42,"filled":0,"resting":5}
{"type":"submit_result","req":"r1","status":"REJECTED","reason":"unknown instrument"}
{"type":"cancel_result","req":"r2","ok":true,"i":1,"id":7}
{"type":"modify_result","req":"r3","status":"REPLACED","i":1,"id":7}
```

`WsGateway` (`src/networking/ws_gateway.cpp`) handles these commands and
forwards the matching-engine trade events back through the
`MarketDataPublisher`, so any WS client that aggresses against resting
liquidity will broadcast resulting trades to **all** connected clients,
not just the originating peer.

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
  networking/    line-oriented TCP protocol, BSD-socket gateway,
                 RFC 6455 WebSocket server, WsGateway command bridge
  observability/ metrics (Prometheus + JSON) + structured JSON logger

src/              implementations of the above
examples/         demo_phase1.cpp, simulator.cpp
benchmarks/       bench_phase1.cpp (Google-Benchmark)
dashboard/        SvelteKit SPA for live market-data visualisation,
                  multi-instrument tabs, order-entry panel
tests/            132 unit + stress + integration tests
notes.md          15-phase build plan
```

---

## License

Personal learning project — use at your own risk.