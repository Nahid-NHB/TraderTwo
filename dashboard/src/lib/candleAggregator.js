// src/lib/candleAggregator.js
//
// Buckets incoming trades into OHLC candles at a fixed interval. The C++
// matching engine only emits discrete trades + top-of-book snapshots, so
// we synthesise candles by walking the trade stream in real time.
//
// A trade at time t with price p fills into bucket floor(t / intervalMs).
// The bucket open = first price, close = last price, high/low are the
// running extremes, volume sums the qty.

const DEFAULT_INTERVAL_MS = 1000;

export function createCandleAggregator(intervalMs = DEFAULT_INTERVAL_MS) {
  /** @type {Map<number, {time: number, open: number, high: number, low: number, close: number, volume: number}>} */
  const buckets = new Map();
  let lastBucket = -1;

  function bucketKey(time) { return Math.floor(time / intervalMs); }

  function add(trade) {
    const k = bucketKey(trade.ts);
    let b = buckets.get(k);
    if (!b) {
      b = {
        time: k * intervalMs / 1000,  // lightweight-charts wants seconds
        open: trade.px,
        high: trade.px,
        low: trade.px,
        close: trade.px,
        volume: 0
      };
      buckets.set(k, b);
    }
    b.high = Math.max(b.high, trade.px);
    b.low  = Math.min(b.low,  trade.px);
    b.close = trade.px;
    b.volume += trade.qty;
    lastBucket = k;
    return b;
  }

  function snapshot() {
    return [...buckets.values()].sort((a, b) => a.time - b.time);
  }

  function clear() { buckets.clear(); lastBucket = -1; }

  return { add, snapshot, clear, intervalMs };
}