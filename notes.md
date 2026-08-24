# TraderTwo — Production-Style Stock Exchange Engine

## What We Are Building

A low-latency, deterministic stock exchange simulator in **C++20** on Linux, inspired by real electronic exchanges (Nasdaq-style). The system receives orders over TCP, validates them through a risk layer, routes them to a matching engine, maintains per-instrument price-time-priority order books, generates trades, publishes market data, persists an append-only event log, and supports crash recovery via replay.

The matching engine is the heart. Everything else (networking, persistence, market data) wraps around it without polluting its core loop.

---

## Architectural Pillars

1. **Determinism over raw speed.** A single matching thread per instrument processes events strictly in sequence-number order. Reproducible state from event log.
2. **Hot-path is dependency-free.** Matching engine uses only `<atomic>`, `<cstdint>`, `<vector>`, custom intrusive containers. No exceptions, no RTTI in hot path.
3. **Partition by instrument.** Each symbol gets its own order book and (eventually) its own matching worker. Inter-instrument contention is zero.
4. **Append-only event log.** All state changes are derived from replayable events. No in-place DB writes.
5. **Lock-free SPSC queues** between threads (Phase 9). Lock-free where possible, coarse locks where it isn't worth the complexity.

---

## Module Layout

```
TraderTwo/
├── CMakeLists.txt
├── README.md
├── notes.md                       # this file
├── include/tt/
│   ├── common/                    # types, ids, prices, sequence
│   ├── core/                      # order, trade, event, order_type
│   ├── orderbook/                 # book, level, intrusive list
│   ├── matching/                  # matching engine
│   ├── market_data/               # best bid/ask, snapshots, streams
│   ├── risk/                      # pre-trade risk checks
│   ├── persistence/               # event log, replay
│   ├── networking/                # TCP server, protocol parser
│   └── concurrency/               # SPSC queue, worker pool
├── src/
│   └── (mirrors include layout)
├── tests/                         # gtest unit + integration tests
├── benchmarks/                    # google-benchmark scenarios
├── simulator/                     # market traffic generator
└── examples/                      # tiny demo clients
```

---

## Phase Plan (Build Incrementally)

| Phase | Goal | Status |
|------|------|--------|
| 1 | Core domain types + price-time-priority order book | **Done (commit `c02136b`)** |
| **2** | Matching engine (limit + market, partial fills) | **In progress** |
| 3 | Cancel + modify (with priority reset on price change) | pending |
| 4 | Multiple instruments registry | pending |
| 5 | Event system + market-data publisher | pending |
| 6 | TCP networking gateway | pending |
| 7 | Event log + replay-based crash recovery | pending |
| 8 | Risk checks layer | pending |
| 9 | Concurrency: SPSC queues + per-instrument workers | pending |
| 10 | Market simulator (CLI tool) | pending |
| 11 | Google-Benchmark scenarios A–F | pending |
| 12 | Observability: metrics + structured logs | pending |
| 13 | Stress + final optimization pass | pending |

---

## Phase 1 — Domain Model + Order Book

### Why these data structures?

**Price ladder:** `std::map<Price, Level, std::greater<Price>>` for bids, `std::map<Price, Level, std::less<Price>>` for asks.
- Best bid/ask lookup: O(log n) via `begin()`. Beats a linear scan or full sort.
- Insert/cancel a price level: O(log n).
- Memory: O(distinct price levels), typically small.

**Per-level FIFO queue:** intrusive doubly-linked list of `Order` nodes.
- Allocating one node per order avoids moving memory when an order is filled or cancelled mid-list.
- Insert at tail, remove from middle (cancel): O(1).
- Iterator-stable across cancels (the node owns its position).
- Cache-friendly: contiguous next/prev pointers, no `std::list` per-node heap allocator overhead if we use a pool.

**Order lookup map:** `std::unordered_map<OrderId, Order*>` for O(1) cancel/modify by ID.
- Stores pointer to intrusive node, so cancel is O(1) amortized.

**Sequence number:** monotonic `uint64_t` per exchange instance, incremented under the book's lock.
- Wall-clock timestamps are non-monotonic (NTP slews, leap seconds). Sequence is the authoritative tie-breaker.

### Complexity summary

| Op | Time | Notes |
|---|---|---|
| Insert limit order | O(log L) | L = price levels, plus O(1) FIFO append |
| Cancel by id | O(1) avg | hashmap + intrusive unlink |
| Best bid/ask | O(log L) | map begin() |
| Match incoming order | O(M log L) | M = fills across levels |
| Top-N depth | O(N log L) | walk N best levels |

### What Phase 1 includes

- `common/`: `Price`, `Quantity`, `OrderId`, `TraderId`, `InstrumentId`, `Sequence`, `Timestamp`.
- `core/`: `Order`, `OrderType`, `Side`, `OrderStatus`, `Trade`, `Event`.
- `orderbook/`: `OrderBook` with bid/ask ladders, intrusive list, cancel/insert/get-best.
- `tests/`: gtest unit tests for the order book (insertion, cancel, price priority, FIFO, best quote).
- `CMakeLists.txt` at root that builds the static library, tests, and a tiny demo binary.

### What Phase 1 does NOT include

No matching yet (Phase 2). No networking, persistence, concurrency. The book only holds and exposes orders.

---

## Phase 2 — Matching Engine

### What was built

- `OrderBook` extended with match-time helpers:
  - `Level::reduce_front(qty)` — fills the front-of-queue order, unlinks if fully consumed, **keeps the residual at HEAD on partial fill** (preserves time priority — matches NYSE/CME convention).
  - `OrderBook::fill(id, qty)` — drives a fill by ID; returns whether the order is still alive.
  - `OrderBook::best_opposite_{order,price}(side)` — fast opposite-side access.
  - `OrderBook::top_bid()/top_ask()` — snapshot.
- `MatchingEngine` (`include/tt/matching/matching_engine.hpp`):
  - One `OrderBook` per `InstrumentId`; registry via `register_instrument`.
  - `submit(order, sink)` validates, assigns sequence, runs the match loop, rests GTC residuals, drops IOC/market remainders.
  - `submit_limit(...)` helper for tests/benches.
  - `cancel(instrument, id)` proxies to the book.
  - `TradeSink` virtual interface with `CollectingSink` reference impl for tests.
- Tests: 20 gtest cases covering exact/partial/multi-level fills, price priority, FIFO, market sweep, IOC, validation rejects, sequence monotonicity, multi-instrument isolation, passive-price execution.

### Matching algorithm (price-time priority)

```
WHILE taker.remaining > 0:
    best_opposite = opposite_book.best_price()
    IF best_opposite invalid OR (limit AND !crosses): BREAK
    resting = opposite_book.best_order()
    fill_qty = min(taker.remaining, resting.remaining)
    opposite_book.fill(resting.id, fill_qty)
    taker.remaining -= fill_qty
    emit Trade(taker.id, resting.id, fill_qty, exec_price = best_opposite)

IF taker.remaining > 0:
    IF market OR IOC: drop remainder
    ELSE (limit GTC): rest into book
```

Trade price is always the **passive (resting) price** — no price improvement from aggressor side. Sequence numbers are authoritative for FIFO tie-breaks.

### Complexity

| Operation | Time |
|---|---|
| Match (M fills across L levels) | O(M log L + M) |
| Insert resting order | O(log L) |
| Cancel by ID | O(1) avg |
| Best bid/ask | O(log L) |
| Multi-instrument dispatch | O(1) via hashmap |

### Design decisions documented

1. **Residual stays at HEAD on partial fill** — keeps time priority for the order that was there first; matches conventional equity exchanges.
2. **Trade price = passive price** — never the aggressor's price; protects resting orders from price-walking.
3. **Sequence numbers consumed by both orders and trades** — keeps a single global monotonic source of truth (useful for replay and tie-breaks).
4. **`TradeSink` is virtual, not std::function** — avoids a virtual call per trade? Actually it IS virtual; we pay one indirection per trade because the consumer (network thread, persistence) wants polymorphism. The hot path could switch to a function pointer later if profiling demands it.

---

## Commit Strategy

After every meaningful working increment:
1. `git add` the changed files only.
2. Commit with a focused message: e.g. `phase1: add core domain types`, `phase1: add order book with price-time priority`, `phase1: add unit tests for order book`.
3. Run the build + tests before committing.

---