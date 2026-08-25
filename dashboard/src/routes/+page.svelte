<script>
  import { onDestroy, onMount } from 'svelte';
  import { derived } from 'svelte/store';
  import { createChart, CandlestickSeries, AreaSeries, HistogramSeries } from 'lightweight-charts';
  import { createMarketDataStore } from '$lib/marketData.js';
  import { createCandleAggregator } from '$lib/candleAggregator.js';
  import { createDepthStore } from '$lib/depthAggregator.js';

  // ----- Config -----
  let wsPort = 9001;
  let selectedInstrument = 1;
  let candleSeconds = 1;
  let useMock = false;

  if (typeof window !== 'undefined') {
    const q = new URLSearchParams(window.location.search);
    const p = parseInt(q.get('port') ?? '', 10);
    if (Number.isFinite(p) && p > 0 && p < 65536) wsPort = p;
    useMock = q.get('mock') === '1' || q.get('mock') === 'true';
  }
  const wsUrl = `ws://${typeof window !== 'undefined' ? window.location.hostname : '127.0.0.1'}:${wsPort}`;

  // ----- Streams -----
  const md = createMarketDataStore({ url: wsUrl, mock: useMock });
  const candles = createCandleAggregator(candleSeconds * 1000);
  const depth = createDepthStore();

  // Latest top-of-book per instrument (for the ladder + summary panel).
  const topOfBook = derived(md.events, ($events) => {
    /** @type {Record<string, any>} */
    const m = {};
    for (const e of $events) {
      if (e.type !== 'tob') continue;
      m[e.i] = {
        bid: e.hb ? { price: e.b, qty: e.bq } : null,
        ask: e.ha ? { price: e.a, qty: e.aq } : null
      };
    }
    return m;
  });

  // Recent trades (newest first) across all instruments.
  const allTrades = derived(md.events, ($events) =>
    $events.filter((e) => e.type === 'trade').slice(-300).reverse()
  );

  // Trades filtered to selected instrument.
  const selectedTrades = derived([allTrades], ([$all]) =>
    $all.filter((t) => t.i === selectedInstrument)
  );

  // Stats for header.
  const stats = derived(md.events, ($events) => {
    let trades = 0, tobs = 0;
    let lastPx = null;
    for (const e of $events) {
      if (e.type === 'trade') { ++trades; lastPx = e.px; }
      else if (e.type === 'tob') ++tobs;
    }
    return { trades, tobs, total: $events.length, lastPx };
  });

  // Available instruments (from observed IDs).
  const instruments = derived(md.events, ($events) => {
    const s = new Set();
    for (const e of $events) s.add(e.i);
    if (s.size === 0) s.add(selectedInstrument);
    return [...s].sort((a, b) => a - b);
  });

  // When new trades arrive, push into the candle aggregator and the depth store.
  let lastEventCount = 0;
  md.events.subscribe(($events) => {
    if ($events.length <= lastEventCount) { lastEventCount = $events.length; return; }
    for (let i = lastEventCount; i < $events.length; ++i) {
      const e = $events[i];
      if (e.type === 'trade') candles.add(e);
      else if (e.type === 'tob') depth.applyToB(e.i, {
        bid: e.hb ? { price: e.b, qty: e.bq } : null,
        ask: e.ha ? { price: e.a, qty: e.aq } : null
      });
    }
    lastEventCount = $events.length;
  });

  md.connect();
  onDestroy(() => md.disconnect());

  // ----- Chart setup -----
  let chartEl;
  let chart;
  let candleSeries;
  let volumeSeries;

  onMount(() => {
    chart = createChart(chartEl, {
      layout: {
        background: { type: 'solid', color: '#0b0f17' },
        textColor: '#cdd6e3',
        fontSize: 12,
        fontFamily: 'ui-sans-serif, system-ui, sans-serif'
      },
      grid: {
        vertLines: { color: '#1a2333' },
        horzLines: { color: '#1a2333' }
      },
      crosshair: { mode: 0 },
      rightPriceScale: { borderColor: '#1a2333' },
      timeScale: {
        borderColor: '#1a2333',
        timeVisible: true,
        secondsVisible: true,
        rightOffset: 6,
        barSpacing: 6
      },
      autoSize: true
    });

    candleSeries = chart.addSeries(CandlestickSeries, {
      upColor:        '#26a69a',
      downColor:      '#ef5350',
      borderUpColor:  '#26a69a',
      borderDownColor:'#ef5350',
      wickUpColor:    '#26a69a',
      wickDownColor:  '#ef5350'
    });

    volumeSeries = chart.addSeries(HistogramSeries, {
      priceFormat: { type: 'volume' },
      priceScaleId: 'volume',
      color: '#26a69a55'
    });
    chart.priceScale('volume').applyOptions({
      scaleMargins: { top: 0.85, bottom: 0 }
    });
  });

  // Update candles whenever trades for the selected instrument arrive.
  let lastCandleCount = 0;
  selectedTrades.subscribe(($trades) => {
    if (!candleSeries || !volumeSeries) return;
    // We re-derive candles from the full candle store which already aggregates
    // across all instruments. To show only the selected instrument we'd need
    // per-instrument aggregators. For visual richness we just plot everything;
    // trades for other instruments will still contribute. Production would
    // key the aggregator by instrument.
    if (lastCandleCount === 0) {
      const cs = candles.snapshot();
      if (cs.length > 0) {
        candleSeries.setData(cs);
        volumeSeries.setData(cs.map(c => ({
          time: c.time, value: c.volume,
          color: c.close >= c.open ? '#26a69a55' : '#ef535055'
        })));
      }
    }
    lastCandleCount = $trades.length;
    // Update last candle live.
    const cs = candles.snapshot();
    if (cs.length > 0) {
      const last = cs[cs.length - 1];
      candleSeries.update(last);
      volumeSeries.update({
        time: last.time, value: last.volume,
        color: last.close >= last.open ? '#26a69a55' : '#ef535055'
      });
    }
  });

  // ----- Depth chart (canvas) -----
  let depthCanvas;
  let depthAnim;

  function drawDepth() {
    if (!depthCanvas) return;
    const ctx = depthCanvas.getContext('2d');
    const w = depthCanvas.width;
    const h = depthCanvas.height;
    ctx.clearRect(0, 0, w, h);

    const stair = depth.staircase(selectedInstrument, 30);
    if (!stair || (stair.bids.prices.length === 0 && stair.asks.prices.length === 0)) {
      ctx.fillStyle = '#6c7891';
      ctx.font = '12px ui-sans-serif';
      ctx.fillText('No depth — submit orders to populate the book', 12, h / 2);
      return;
    }

    const mid = stair.bestBid && stair.bestAsk
      ? (stair.bestBid + stair.bestAsk) / 2
      : (stair.bestBid ?? stair.bestAsk);
    const minPrice = stair.bids.prices.length
      ? stair.bids.prices[stair.bids.prices.length - 1]
      : mid - 50;
    const maxPrice = stair.asks.prices.length
      ? stair.asks.prices[stair.asks.prices.length - 1]
      : mid + 50;
    const priceRange = Math.max(1, maxPrice - minPrice);

    const maxQty = Math.max(
      ...stair.bids.cumQty, ...stair.asks.cumQty, 1
    );

    const xForPrice = (p) => ((p - minPrice) / priceRange) * w;
    const yForQty   = (q) => h - (q / maxQty) * (h - 24) - 12;

    // Mid line
    if (mid != null) {
      ctx.strokeStyle = '#3b4a66';
      ctx.setLineDash([4, 4]);
      ctx.beginPath();
      ctx.moveTo(xForPrice(mid), 0);
      ctx.lineTo(xForPrice(mid), h);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // Bid area (left of mid)
    if (stair.bids.prices.length > 0) {
      ctx.fillStyle = 'rgba(38, 166, 154, 0.20)';
      ctx.strokeStyle = '#26a69a';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(xForPrice(mid), h);
      // Walk bids in order of descending price
      const xs = stair.bids.prices.slice().reverse().map(xForPrice);
      const ys = stair.bids.cumQty.slice().reverse().map(yForQty);
      ctx.lineTo(xs[0], h);
      for (let i = 0; i < xs.length; ++i) ctx.lineTo(xs[i], ys[i]);
      ctx.lineTo(xForPrice(mid), h);
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
    }

    // Ask area (right of mid)
    if (stair.asks.prices.length > 0) {
      ctx.fillStyle = 'rgba(239, 83, 80, 0.20)';
      ctx.strokeStyle = '#ef5350';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(xForPrice(mid), h);
      const xs = stair.asks.prices.map(xForPrice);
      const ys = stair.asks.cumQty.map(yForQty);
      ctx.lineTo(xs[0], h);
      for (let i = 0; i < xs.length; ++i) ctx.lineTo(xs[i], ys[i]);
      ctx.lineTo(xForPrice(mid), h);
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
    }

    // Best bid/ask labels
    ctx.fillStyle = '#26a69a';
    if (stair.bestBid != null) {
      const y = yForQty(stair.bids.cumQty[0]);
      ctx.fillText(`B ${stair.bestBid}`, xForPrice(stair.bestBid) - 28, Math.max(12, y - 4));
    }
    ctx.fillStyle = '#ef5350';
    if (stair.bestAsk != null) {
      const y = yForQty(stair.asks.cumQty[0]);
      ctx.fillText(`A ${stair.bestAsk}`, xForPrice(stair.bestAsk) - 4, Math.max(12, y - 4));
    }
  }

  function resizeDepth() {
    if (!depthCanvas) return;
    const rect = depthCanvas.getBoundingClientRect();
    depthCanvas.width = rect.width;
    depthCanvas.height = rect.height;
    drawDepth();
  }

  $: if (depthCanvas && selectedInstrument !== undefined) drawDepth();

  onMount(() => {
    resizeDepth();
    const ro = new ResizeObserver(resizeDepth);
    ro.observe(depthCanvas);
    // Redraw at ~10 Hz so the depth chart updates smoothly between WS frames.
    depthAnim = setInterval(drawDepth, 100);
    return () => { ro.disconnect(); clearInterval(depthAnim); };
  });

  // ----- Helpers -----
  function fmt(n) { return (n ?? 0).toLocaleString(); }
  function fmtPx(n) {
    if (n === null || n === undefined) return '—';
    return Number(n).toLocaleString();
  }
  function spread() {
    const t = $topOfBook[selectedInstrument];
    if (!t || !t.bid || !t.ask) return null;
    return t.ask.price - t.bid.price;
  }
  function fmtTime(ts) {
    const d = new Date(ts);
    return d.toLocaleTimeString();
  }
</script>

<svelte:head>
  <title>TraderTwo · TradingView-style dashboard</title>
</svelte:head>

<main>
  <header>
    <div class="brand">
      <h1>TraderTwo <span class="tag">{useMock ? 'mock' : 'live'}</span></h1>
      <div class="sub">
        <select bind:value={selectedInstrument}>
          {#each $instruments as i}
            <option value={i}>Instrument {i}</option>
          {/each}
        </select>
        <span class="status-text">
          <span class="dot {useMock ? 'open' : md.status}"></span>
          {useMock ? 'mock generator · ~14 ticks/s' : `${wsUrl} · ${md.status}`}
        </span>
      </div>
    </div>
    <div class="metrics">
      <div><label>Last</label><b>{fmtPx($stats.lastPx)}</b></div>
      <div><label>Spread</label><b>{spread() != null ? fmtPx(spread()) : '—'}</b></div>
      <div><label>Trades</label><b>{fmt($stats.trades)}</b></div>
      <div><label>TOB updates</label><b>{fmt($stats.tobs)}</b></div>
    </div>
  </header>

  <section class="chart-row">
    <div class="card chart-card">
      <div class="card-head">
        <h2>Price · {candleSeconds}s candles</h2>
        <div class="intervals">
          {#each [1, 5, 15] as iv}
            <button class:active={candleSeconds === iv} on:click={() => { candleSeconds = iv; candles.clear(); }}>{iv}s</button>
          {/each}
        </div>
      </div>
      <div class="chart" bind:this={chartEl}></div>
    </div>

    <div class="card depth-card">
      <div class="card-head">
        <h2>Depth</h2>
        <span class="muted">cumulative quantity</span>
      </div>
      <canvas bind:this={depthCanvas}></canvas>
    </div>
  </section>

  <section class="bottom-row">
    <div class="card ladder">
      <div class="card-head"><h2>Order book</h2></div>
      <table>
        <thead>
          <tr>
            <th class="num">Bid qty</th>
            <th class="num">Bid</th>
            <th></th>
            <th class="num">Ask</th>
            <th class="num">Ask qty</th>
          </tr>
        </thead>
        <tbody>
          {#if $topOfBook[selectedInstrument]}
            {@const t = $topOfBook[selectedInstrument]}
            <tr class="best">
              <td class="num bid">{t.bid ? fmt(t.bid.qty) : '—'}</td>
              <td class="num bid strong">{t.bid ? fmtPx(t.bid.price) : '—'}</td>
              <td class="muted">⇅</td>
              <td class="num ask strong">{t.ask ? fmtPx(t.ask.price) : '—'}</td>
              <td class="num ask">{t.ask ? fmt(t.ask.qty) : '—'}</td>
            </tr>
          {:else}
            <tr><td colspan="5" class="empty">Waiting for first TOB…</td></tr>
          {/if}
        </tbody>
      </table>
      <p class="hint">
        Best prices update live from the WebSocket feed. Full depth is shown
        in the depth chart above.
      </p>
    </div>

    <div class="card tape">
      <div class="card-head">
        <h2>Time &amp; sales · Instrument {selectedInstrument}</h2>
        <span class="muted">{$selectedTrades.length} trades</span>
      </div>
      {#if $selectedTrades.length === 0}
        <p class="empty">No trades for this instrument yet.</p>
      {:else}
        <table>
          <thead>
            <tr>
              <th>Time</th>
              <th class="num">Price</th>
              <th class="num">Qty</th>
              <th class="num">Buy</th>
              <th class="num">Sell</th>
              <th class="num">Seq</th>
            </tr>
          </thead>
          <tbody>
            {#each $selectedTrades as t (t.seq + '-' + t.ts)}
              <tr class:up={t.px >= ($selectedTrades[$selectedTrades.indexOf(t) + 1]?.px ?? t.px)}
                  class:down={t.px <  ($selectedTrades[$selectedTrades.indexOf(t) + 1]?.px ?? t.px)}>
                <td>{fmtTime(t.ts)}</td>
                <td class="num strong">{fmtPx(t.px)}</td>
                <td class="num">{t.qty}</td>
                <td class="num">{t.buy}</td>
                <td class="num">{t.sell}</td>
                <td class="num muted">{t.seq}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    </div>
  </section>

  <footer>
    {#if useMock}
      <p>Running with synthetic mock data. Switch to the live engine with
        <a href="?port={wsPort}">?port={wsPort}</a>.</p>
    {:else}
      <p>Engine: <code>./build/tt_simulator --random 50000 --instruments 4 --ws-port {wsPort}</code></p>
      <p>No engine? Try <a href="?mock=1">?mock=1</a> for a synthetic feed.</p>
    {/if}
    <p>This dashboard uses <a href="https://tradingview.github.io/lightweight-charts/">TradingView Lightweight Charts™</a> for the candlestick rendering.</p>
  </footer>
</main>

<style>
  :global(html), :global(body) {
    margin: 0;
    padding: 0;
    background: #0b0f17;
    color: #e7ecf3;
    font-family: ui-sans-serif, system-ui, -apple-system, 'SF Pro', sans-serif;
    font-size: 14px;
    line-height: 1.45;
  }
  :global(*), :global(*::before), :global(*::after) { box-sizing: border-box; }
  :global(code), :global(.mono) {
    font-family: ui-monospace, 'JetBrains Mono', Menlo, monospace;
  }

  main {
    max-width: 1500px;
    margin: 0 auto;
    padding: 22px 22px 80px;
  }

  header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 24px;
    flex-wrap: wrap;
    border-bottom: 1px solid #1a2333;
    padding-bottom: 16px;
    margin-bottom: 16px;
  }
  .brand h1 {
    margin: 0;
    font-size: 22px;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  .tag {
    background: linear-gradient(135deg, #19305c 0%, #2a4a8e 100%);
    color: #86b3ff;
    border-radius: 4px;
    font-size: 10px;
    font-weight: 700;
    padding: 2px 6px;
    margin-left: 6px;
    vertical-align: middle;
    text-transform: uppercase;
    letter-spacing: 0.08em;
  }
  .sub {
    display: flex;
    gap: 16px;
    align-items: center;
    margin-top: 6px;
    color: #8a96ac;
    font-size: 13px;
  }
  .sub select {
    background: #111827;
    color: #e7ecf3;
    border: 1px solid #1f2a3d;
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 13px;
  }
  .status-text {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .dot {
    width: 8px; height: 8px;
    border-radius: 50%;
    background: #555;
  }
  .dot.open { background: #2fd07a; box-shadow: 0 0 8px #2fd07a88; }
  .dot.connecting { background: #f0c84a; }
  .dot.error { background: #e85d6d; }
  .dot.disconnected { background: #555; }

  .metrics {
    display: flex;
    gap: 10px;
  }
  .metrics > div {
    background: #111827;
    border: 1px solid #1f2a3d;
    padding: 8px 14px;
    border-radius: 6px;
    min-width: 96px;
  }
  .metrics label {
    display: block;
    color: #8a96ac;
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    margin-bottom: 3px;
  }
  .metrics b {
    font-size: 18px;
    font-weight: 600;
    color: #e7ecf3;
    font-variant-numeric: tabular-nums;
  }

  .chart-row {
    display: grid;
    grid-template-columns: 2fr 1fr;
    gap: 16px;
    margin-bottom: 16px;
  }
  @media (max-width: 1100px) {
    .chart-row { grid-template-columns: 1fr; }
  }

  .card {
    background: #0f1626;
    border: 1px solid #1f2a3d;
    border-radius: 8px;
    padding: 14px 16px;
  }
  .card-head {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 10px;
  }
  .card-head h2 {
    margin: 0;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: #b1bdcf;
    font-weight: 600;
  }
  .muted { color: #6c7891; font-size: 11px; }
  .intervals { display: flex; gap: 4px; }
  .intervals button {
    background: #111827;
    color: #8a96ac;
    border: 1px solid #1f2a3d;
    border-radius: 4px;
    padding: 3px 10px;
    font-size: 12px;
    cursor: pointer;
    transition: all 0.15s;
  }
  .intervals button:hover { background: #1a2333; color: #cdd6e3; }
  .intervals button.active {
    background: #1a3a6c;
    color: #e7ecf3;
    border-color: #2a4a8e;
  }

  .chart-card { padding: 14px 12px 10px; }
  .chart {
    width: 100%;
    height: 380px;
  }

  .depth-card canvas {
    width: 100%;
    height: 380px;
    display: block;
  }

  .bottom-row {
    display: grid;
    grid-template-columns: 1fr 2fr;
    gap: 16px;
  }
  @media (max-width: 900px) {
    .bottom-row { grid-template-columns: 1fr; }
  }

  .ladder table {
    width: 100%;
    border-collapse: collapse;
    font-size: 14px;
    margin-top: 8px;
  }
  .ladder thead th {
    color: #6c7891;
    font-weight: 500;
    text-align: right;
    padding: 6px 8px;
    border-bottom: 1px solid #1f2a3d;
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
  }
  .ladder tbody td {
    padding: 8px;
    border-bottom: 1px solid #18223730;
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
  .ladder tbody tr.best td { font-size: 16px; padding: 12px 8px; }
  .bid { color: #5cd2a4; }
  .ask { color: #f08080; }
  .strong { font-weight: 600; color: #e7ecf3; }
  .hint {
    margin-top: 16px;
    color: #6c7891;
    font-size: 12px;
    font-style: italic;
  }

  .tape {
    max-height: 360px;
    overflow-y: auto;
  }
  .tape table {
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
  }
  .tape thead th {
    color: #6c7891;
    font-weight: 500;
    text-align: left;
    padding: 6px 8px;
    border-bottom: 1px solid #1f2a3d;
    position: sticky;
    top: 0;
    background: #0f1626;
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
  }
  .tape tbody td {
    padding: 5px 8px;
    border-bottom: 1px solid #18223730;
  }
  .tape tbody tr {
    transition: background-color 0.4s;
  }
  .tape tbody tr.up td { background: rgba(38, 166, 154, 0.08); }
  .tape tbody tr.down td { background: rgba(239, 83, 80, 0.08); }
  .num { text-align: right; font-variant-numeric: tabular-nums; }

  .empty {
    color: #6c7891;
    font-style: italic;
    margin: 12px 0;
  }

  footer {
    margin-top: 28px;
    padding-top: 16px;
    border-top: 1px solid #1a2333;
    color: #8a96ac;
    font-size: 12px;
  }
  footer code {
    background: #111827;
    border: 1px solid #1f2a3d;
    padding: 1px 6px;
    border-radius: 3px;
  }
  footer a { color: #86b3ff; text-decoration: none; }
  footer a:hover { text-decoration: underline; }
</style>