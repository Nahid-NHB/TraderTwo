# TraderTwo Dashboard

A TradingView-styled SvelteKit SPA that connects to the WebSocket
market-data endpoint exposed by `tt_simulator --ws-port P` and visualises
live trades + top-of-book in a familiar broker-terminal layout. The
dashboard also submits typed orders back to the engine over the same
WebSocket connection.

## Layout

```
┌───────────────────────────────────────────────────────────────┐
│ TraderTwo [LIVE]  Inst 1 9800·9900  Inst 2  ...   ● open     │
│ Last | Spread | Trades | TOB updates                          │
├──────────────────────────────────────────┬────────────────────┤
│                                          │                    │
│   Candlestick chart (TradingView)        │   Depth chart      │
│   + volume histogram                     │   (bid/ask area)   │
│                                          │                    │
├──────────────────────────────────────────┴────────────────────┤
│  Watch grid — every instrument's BBO side-by-side             │
├──────────────────────────────────────────┬────────────────────┤
│  Best bid/ask ladder  │  Time & sales   │  Order entry panel  │
└──────────────────────────────────────────┴────────────────────┘
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
different one. The instrument tabs are populated automatically from
whichever instruments the engine emits events for. Pass `?mock=1` to
synthesise a feed in the browser without an engine (mock mode disables
order entry).

## Features

- **Multi-instrument tabs** — a tab per instrument in the header, each
  showing a live BBO (bid · ask). Click to focus that instrument in the
  charts/ladder/tape below.
- **Watch grid** — every instrument's best bid / ask / spread in a single
  panel; click any cell to focus it.
- **Live candlestick chart** — 1s / 5s / 15s candles synthesised on the fly
  from the trade feed, with a volume histogram overlay.
- **Depth chart** — bid/ask cumulative-quantity staircase that updates
  every 100ms, with a centre line at the mid-price.
- **Order-book ladder** — best bid/ask with bid and ask quantities,
  colour-coded in the TradingView teal/red palette.
- **Time & sales** — tape of recent trades for the selected instrument,
  with green/red flash highlighting for up/down ticks relative to the
  previous trade.
- **Order entry panel** — submit / cancel / modify orders and ping the
  engine over the same WebSocket connection. Last reply is rendered as a
  colour-coded JSON card so accept/reject is visible inline.
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

The C++ server sends three JSON message types as text frames: trades,
top-of-book updates, and command replies. The dashboard listens for all
three but only consumes trades + TOBs for visualisation.

```json
{"type":"trade","i":1,"buy":2,"sell":1,"px":100,"qty":3,"seq":2}
{"type":"tob","i":1,"b":100,"bq":5,"a":101,"aq":3,"hb":1,"ha":1}
{"type":"submit_result","req":"r1","status":"ACCEPTED","id":7,"i":1,"seq":42}
```

| Field | Meaning |
|---|---|
| `i`   | instrument id |
| `b/a` | bid/ask price in ticks |
| `bq/aq` | bid/ask quantity |
| `hb/ha` | 1 if side has a quote, else 0 |
| `buy/sell` | resting order ids |
| `seq` | matching-engine sequence number |
| `req` | opaque client-supplied request id (echoed in replies) |

Client → server commands (sent as JSON text frames):

```json
{"type":"ping","req":"abc"}
{"type":"submit","req":"r1","i":1,"trader":99,"side":0,"px":100,"qty":5,"tif":"GTC"}
{"type":"cancel","req":"r2","i":1,"id":7}
{"type":"modify","req":"r3","i":1,"id":7,"qty":3,"px":101}
```

`side`: `0` = buy, `1` = sell. `tif`: `GTC` | `IOC` | `FOK`. The
dashboard's order-entry panel sets these from UI controls and prints
each reply inline.
