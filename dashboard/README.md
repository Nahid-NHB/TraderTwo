# TraderTwo Dashboard

A TradingView-styled SvelteKit SPA that connects to the WebSocket
market-data endpoint exposed by `tt_simulator --ws-port P` and visualises
live trades + top-of-book in a familiar broker-terminal layout.

## Layout

```
┌───────────────────────────────────────────────────────────────┐
│ TraderTwo [LIVE]  Inst: ▾   ws://127.0.0.1:9001  ● open      │
│ Last | Spread | Trades | TOB updates                          │
├──────────────────────────────────────────┬────────────────────┤
│                                          │                    │
│   Candlestick chart (TradingView)        │   Depth chart      │
│   + volume histogram                     │   (bid/ask area)   │
│                                          │                    │
├──────────────────────────────────────────┴────────────────────┤
│  Best bid/ask ladder        │  Time & sales (filtered)        │
└───────────────────────────────────────────────────────────────┘
```

Charts are powered by [TradingView Lightweight Charts™](https://tradingview.github.io/lightweight-charts/),
the same open-source rendering library TradingView ships for embeds.

## Run it

```sh
# Terminal 1 — start the matching engine with a WS endpoint on :9001
./build/tt_simulator --random 50000 --instruments 4 --ws-port 9001

# Terminal 2 — start the dashboard (dev mode)
cd dashboard
npm install
npm run dev
# open http://127.0.0.1:5173/?port=9001
```

The default WS port is `9001`. Pass `?port=N` in the URL to point at a
different one. The instrument dropdown is populated automatically from
whichever instruments the engine emits events for.

## Features

- **Live candlestick chart** — 1s / 5s / 15s candles synthesised on the fly
  from the trade feed, with a volume histogram overlay.
- **Depth chart** — bid/ask cumulative-quantity staircase that updates
  every 100ms, with a centre line at the mid-price.
- **Order-book ladder** — best bid/ask with bid and ask quantities,
  colour-coded in the TradingView teal/red palette.
- **Time & sales** — tape of recent trades for the selected instrument,
  with green/red flash highlighting for up/down ticks relative to the
  previous trade.
- **Header metrics** — last price, spread, total trade count, TOB-update
  count, and connection status.

## Build static output

```sh
cd dashboard
npm install
npm run build
# Output goes to dashboard/build/ as a static SPA. Serve it with any
# static file server, e.g.  npx serve dashboard/build
```

## Message format

The C++ server sends two JSON message types as text frames:

```json
{"type":"trade","i":1,"buy":2,"sell":1,"px":100,"qty":3,"seq":2}
{"type":"tob","i":1,"b":100,"bq":5,"a":101,"aq":3,"hb":1,"ha":1}
```

| Field | Meaning |
|---|---|
| `i`   | instrument id |
| `b/a` | bid/ask price in ticks |
| `bq/aq` | bid/ask quantity |
| `hb/ha` | 1 if side has a quote, else 0 |
| `buy/sell` | resting order ids |
| `seq` | matching-engine sequence number |
