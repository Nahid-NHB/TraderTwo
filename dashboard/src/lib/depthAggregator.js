// src/lib/depthAggregator.js
//
// Tracks per-instrument bid/ask snapshots over time so we can draw a
// depth visualisation: stacked cumulative quantity at each price level.
//
// Each level (price) accumulates the latest resting quantity. The depth
// chart plots:
//   - bids: descending price, cumulative quantity (left side)
//   - asks: ascending price, cumulative quantity (right side)
// forming the classic "staircase" depth chart.

export function createDepthStore() {
  /** @type {Record<string, {bids: Map<number, number>, asks: Map<number, number>}>} */
  const books = {};

  function update(instrument, side, price, qty) {
    if (!books[instrument]) {
      books[instrument] = { bids: new Map(), asks: new Map() };
    }
    const map = side === 'bid' ? books[instrument].bids : books[instrument].asks;
    if (qty <= 0) map.delete(price);
    else map.set(price, qty);
  }

  function applyToB(instrument, tob) {
    if (!tob) return;
    if (tob.bid) update(instrument, 'bid', tob.bid.price, tob.bid.qty);
    else clearSide(instrument, 'bid');
    if (tob.ask) update(instrument, 'ask', tob.ask.price, tob.ask.qty);
    else clearSide(instrument, 'ask');
  }

  function clearSide(instrument, side) {
    if (!books[instrument]) return;
    const map = side === 'bid' ? books[instrument].bids : books[instrument].asks;
    map.clear();
  }

  /**
   * Returns staircase arrays:
   *   bids: { prices: [...], cumQty: [...], sortedDesc }
   *   asks: { prices: [...], cumQty: [...], sortedAsc }
   */
  function staircase(instrument, levels = 50) {
    const b = books[instrument];
    if (!b) return null;
    const bidPrices = [...b.bids.keys()].sort((a, b) => b - a).slice(0, levels);
    const askPrices = [...b.asks.keys()].sort((a, b) => a - b).slice(0, levels);
    let cum = 0;
    const bids = {
      prices: bidPrices,
      cumQty: bidPrices.map(p => { cum += b.bids.get(p); return cum; })
    };
    cum = 0;
    const asks = {
      prices: askPrices,
      cumQty: askPrices.map(p => { cum += b.asks.get(p); return cum; })
    };
    return { bids, asks, bestBid: bidPrices[0], bestAsk: askPrices[0] };
  }

  return { update, applyToB, staircase };
}