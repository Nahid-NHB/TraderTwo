// src/lib/marketData.js
//
// Two modes for the dashboard data source:
//
//   1. WebSocket — connects to the C++ matching engine (default).
//   2. Mock      — synthesises trades + top-of-book updates locally so the
//                  UI can be exercised without an engine. Activated by
//                  `?mock=1` in the URL.
//
// Both modes feed the same events store, so the rest of the dashboard is
// identical. The mock runs a small multi-instrument market model:
//
//   - N instruments, each with its own mean-reverting price around an anchor.
//   - Per-instrument book: a stack of price levels with quantities that
//     decay over time and get replenished on each trade.
//   - Trade generator: biased toward the current best quote so the candles
//     trend realistically.
//
// Event format (matches what the C++ WS server sends):
//
//   {"type":"trade","i":<id>,"buy":<id>,"sell":<id>,"px":<p>,"qty":<q>,"seq":<n>}
//   {"type":"tob",  "i":<id>,"b":<p>,"bq":<q>,"a":<p>,"aq":<q>,"hb":<0|1>,"ha":<0|1>}

import { writable } from 'svelte/store';

const MAX_LOG = 4096;

// ---------------------------------------------------------------------------
// WebSocket source
// ---------------------------------------------------------------------------
function createWsSource(url, events, status, socket) {
  function connect() {
    status.set('connecting');
    let ws;
    try { ws = new WebSocket(url); }
    catch { status.set('error'); return; }

    ws.addEventListener('open', () => {
      status.set('open');
      socket.set(ws);
    });
    ws.addEventListener('error', () => status.set('error'));
    ws.addEventListener('close', () => {
      status.set('disconnected');
      socket.set(null);
    });

    ws.addEventListener('message', (ev) => {
      let parsed;
      try { parsed = JSON.parse(ev.data); }
      catch { return; }
      pushEvent(events, parsed);
    });

    return () => { if (ws.readyState <= WebSocket.OPEN) ws.close(); };
  }
  return { connect };
}

// ---------------------------------------------------------------------------
// Mock source — a small market simulator running entirely in the browser.
// ---------------------------------------------------------------------------
function createMockSource(events, status, opts = {}) {
  const instruments = opts.instruments ?? [1, 2, 3, 4];
  const anchorPrices = opts.anchorPrices ?? [10000, 25000, 50000, 80000];
  const tickHz = opts.tickHz ?? 12;          // events per second (approx)
  const minQty = opts.minQty ?? 1;
  const maxQty = opts.maxQty ?? 25;

  // Per-instrument state.
  const state = instruments.map((id, idx) => {
    const anchor = anchorPrices[idx % anchorPrices.length];
    return {
      id,
      anchor,
      mid: anchor,
      // A tiny synthetic book — each side is a sorted array of price levels.
      bids: levelBook(anchor - 5, 8),
      asks: levelBook(anchor + 5, 8),
      seq: 0
    };
  });

  function levelBook(start, count) {
    const levels = [];
    for (let i = 0; i < count; ++i) levels.push({ price: start + i, qty: rand(50, 200) });
    return levels;
  }

  function rand(min, max) { return Math.floor(min + Math.random() * (max - min)); }
  function meanRev(x, anchor, strength) {
    return x + (anchor - x) * strength + (Math.random() - 0.5) * (anchor * 0.001);
  }

  function emitTrade(s) {
    // Decide side: 50/50 buy vs sell at the touch, biased toward filling
    // against the best opposite price.
    const buyCrossover = Math.random() < 0.5;
    const takerQty = rand(minQty, maxQty);
    let px, qty;
    if (buyCrossover) {
      px = s.asks[0].price;
      qty = Math.min(takerQty, s.asks[0].qty);
      if (qty <= 0) return;
      s.asks[0].qty -= qty;
      if (s.asks[0].qty === 0) s.asks.shift();
    } else {
      px = s.bids[0].price;
      qty = Math.min(takerQty, s.bids[0].qty);
      if (qty <= 0) return;
      s.bids[0].qty -= qty;
      if (s.bids[0].qty === 0) s.bids.shift();
    }
    ++s.seq;
    const buyOrder  = rand(1000, 99999);
    const sellOrder = rand(1000, 99999);
    pushEvent(events, {
      type: 'trade',
      i: s.id,
      buy: buyOrder,
      sell: sellOrder,
      px,
      qty,
      seq: s.seq
    });
    // Refill the consumed level and nudge mid.
    if (buyCrossover) {
      const top = s.asks[0];
      if (top) top.qty += rand(20, 120);
      else s.asks.push({ price: s.mid + 1, qty: rand(50, 200) });
    } else {
      const top = s.bids[0];
      if (top) top.qty += rand(20, 120);
      else s.bids.unshift({ price: s.mid - 1, qty: rand(50, 200) });
    }
    s.mid = meanRev(px, s.anchor, 0.05);
    emitTob(s);
  }

  function emitTob(s) {
    // Maintain best bid/ask by pruning empty levels.
    while (s.bids.length && s.bids[0].qty <= 0) s.bids.shift();
    while (s.asks.length && s.asks[0].qty <= 0) s.asks.shift();
    // Top up the book if it's getting thin.
    while (s.bids.length < 5) {
      const last = s.bids[s.bids.length - 1];
      const next = (last ? last.price : Math.floor(s.mid) - 1) - 1;
      s.bids.push({ price: next, qty: rand(50, 200) });
    }
    while (s.asks.length < 5) {
      const last = s.asks[s.asks.length - 1];
      const next = (last ? last.price : Math.floor(s.mid) + 1) + 1;
      s.asks.push({ price: next, qty: rand(50, 200) });
    }
    const b = s.bids[0];
    const a = s.asks[0];
    pushEvent(events, {
      type: 'tob',
      i: s.id,
      b: b ? b.price : 0,
      bq: b ? b.qty : 0,
      a: a ? a.price : 0,
      aq: a ? a.qty : 0,
      hb: b ? 1 : 0,
      ha: a ? 1 : 0
    });
  }

  function seed() {
    // Emit an initial TOB for every instrument so the UI has data
    // immediately on connect.
    for (const s of state) emitTob(s);
  }

  let timer = null;
  function start() {
    status.set('open');
    seed();
    const intervalMs = Math.max(1, Math.floor(1000 / tickHz));
    timer = setInterval(() => {
      // Pick a random instrument and emit a trade (plus its TOB).
      const s = state[Math.floor(Math.random() * state.length)];
      emitTrade(s);
    }, intervalMs);
  }
  function stop() {
    if (timer) { clearInterval(timer); timer = null; }
    status.set('disconnected');
  }

  return { start, stop };
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------
function pushEvent(events, parsed) {
  events.update((list) => {
    const next = list.length >= MAX_LOG
      ? list.slice(list.length - MAX_LOG + 1)
      : list.slice();
    next.push({ ts: Date.now(), ...parsed });
    return next;
  });
}

function createMarketDataStore({ url, mock }) {
  const status = writable(mock ? 'disconnected' : 'disconnected');
  const events = writable([]);
  const socket = writable(null);

  let cleanup = null;
  let mockCtrl = null;

  function connect() {
    if (mock) {
      mockCtrl = createMockSource(events, status, {
        instruments: [1, 2, 3, 4],
        anchorPrices: [10000, 25000, 50000, 80000],
        tickHz: 14
      });
      mockCtrl.start();
    } else {
      const wsSrc = createWsSource(url, events, status, socket);
      cleanup = wsSrc.connect();
    }
  }

  function disconnect() {
    if (mockCtrl) { mockCtrl.stop(); mockCtrl = null; }
    if (cleanup)  { cleanup();    cleanup  = null; }
  }

  return { status, events, socket, connect, disconnect };
}

export { createMarketDataStore, MAX_LOG };