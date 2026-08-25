# TraderTwo Dashboard

A small SvelteKit SPA that connects to the WebSocket market-data endpoint
exposed by `tt_simulator --ws-port P` and renders live trades + top-of-book
updates in a clean dark UI.

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
different one.

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
