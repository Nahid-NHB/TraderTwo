<script>
  import { onDestroy } from 'svelte';
  import { derived } from 'svelte/store';
  import { createMarketDataStore } from '$lib/marketData.js';

  // Configurable via the WS_PORT env var baked into the page (set at build
  // time by Vite) or via a query string ?port=9001.
  let wsPort = 9001;
  if (typeof window !== 'undefined') {
    const q = new URLSearchParams(window.location.search);
    const p = parseInt(q.get('port') ?? '', 10);
    if (Number.isFinite(p) && p > 0 && p < 65536) wsPort = p;
  }
  const wsUrl = `ws://${typeof window !== 'undefined' ? window.location.hostname : '127.0.0.1'}:${wsPort}`;

  const md = createMarketDataStore(wsUrl);

  // Derive trade list and book snapshots from the event log.
  const trades = derived(md.events, ($events) =>
    $events.filter((e) => e.type === 'trade').slice(-200).reverse()
  );

  const books = derived(md.events, ($events) => {
    /** @type {Record<string, any>} */
    const map = {};
    // Walk in chronological order so latest tob wins.
    for (const e of $events) {
      if (e.type !== 'tob') continue;
      map[e.i] = {
        bid: e.hb ? { price: e.b, qty: e.bq } : null,
        ask: e.ha ? { price: e.a, qty: e.aq } : null
      };
    }
    return map;
  });

  // Counts
  const stats = derived(md.events, ($events) => {
    let trades = 0;
    let tobs = 0;
    for (const e of $events) {
      if (e.type === 'trade') ++trades;
      else if (e.type === 'tob') ++tobs;
    }
    return { trades, tobs, total: $events.length };
  });

  md.connect();
  onDestroy(() => md.disconnect());

  function fmt(n) {
    if (n === null || n === undefined) return '—';
    return n.toLocaleString();
  }
  function sideBadge(t) {
    return t.seq ? '●' : '';
  }
  function fmtTime(ts) {
    const d = new Date(ts);
    return d.toLocaleTimeString();
  }
</script>

<svelte:head>
  <title>TraderTwo · Live</title>
</svelte:head>

<main>
  <header>
    <div class="brand">
      <h1>TraderTwo <span class="tag">live</span></h1>
      <p class="sub">WebSocket market-data feed from the matching engine</p>
    </div>
    <div class="status">
      <span class="dot {md.status}"></span>
      <code>{wsUrl}</code>
      <span class="status-text">{md.status}</span>
    </div>
  </header>

  <section class="stats">
    <div><label>Messages</label><b>{fmt($stats.total)}</b></div>
    <div><label>Trades</label><b>{fmt($stats.trades)}</b></div>
    <div><label>Top-of-book updates</label><b>{fmt($stats.tobs)}</b></div>
  </section>

  <section class="grid">
    <div class="card">
      <h2>Top of book</h2>
      {#if Object.keys($books).length === 0}
        <p class="empty">No data yet — submit a few orders with the C++ simulator to see the book move.</p>
      {:else}
        <table>
          <thead>
            <tr>
              <th>Instrument</th>
              <th class="num">Bid qty</th>
              <th class="num">Bid</th>
              <th class="num">Ask</th>
              <th class="num">Ask qty</th>
              <th class="num">Spread</th>
            </tr>
          </thead>
          <tbody>
            {#each Object.entries($books) as [inst, b] (inst)}
              {@const spread = (b.bid && b.ask) ? (b.ask.price - b.bid.price) : null}
              <tr>
                <td class="mono">{inst}</td>
                <td class="num bid">{b.bid ? fmt(b.bid.qty) : '—'}</td>
                <td class="num bid">{b.bid ? fmt(b.bid.price) : '—'}</td>
                <td class="num ask">{b.ask ? fmt(b.ask.price) : '—'}</td>
                <td class="num ask">{b.ask ? fmt(b.ask.qty) : '—'}</td>
                <td class="num">{spread !== null ? fmt(spread) : '—'}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    </div>

    <div class="card">
      <h2>Recent trades <span class="muted">({$trades.length})</span></h2>
      {#if $trades.length === 0}
        <p class="empty">No trades yet.</p>
      {:else}
        <table>
          <thead>
            <tr>
              <th>Time</th>
              <th>Instrument</th>
              <th class="num">Buy</th>
              <th class="num">Sell</th>
              <th class="num">Price</th>
              <th class="num">Qty</th>
              <th class="num">Seq</th>
            </tr>
          </thead>
          <tbody>
            {#each $trades as t (t.seq)}
              <tr>
                <td>{fmtTime(t.ts)}</td>
                <td class="mono">{t.i}</td>
                <td class="num">{t.buy}</td>
                <td class="num">{t.sell}</td>
                <td class="num strong">{t.px}</td>
                <td class="num">{t.qty}</td>
                <td class="num muted">{t.seq}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    </div>
  </section>

  <footer>
    <p>Start the engine with
      <code>./build/tt_simulator --random 1000 --instruments 4 --ws-port {wsPort}</code>
      then open this page. Pass <code>?port=N</code> to point at a different port.</p>
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
    max-width: 1200px;
    margin: 0 auto;
    padding: 28px 24px 80px;
  }

  header {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    gap: 24px;
    flex-wrap: wrap;
    border-bottom: 1px solid #1a2333;
    padding-bottom: 18px;
    margin-bottom: 18px;
  }
  .brand h1 {
    margin: 0;
    font-size: 22px;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  .tag {
    background: #19305c;
    color: #86b3ff;
    border-radius: 4px;
    font-size: 11px;
    font-weight: 600;
    padding: 2px 6px;
    margin-left: 6px;
    vertical-align: middle;
    text-transform: uppercase;
    letter-spacing: 0.06em;
  }
  .sub { margin: 4px 0 0; color: #8a96ac; font-size: 13px; }

  .status {
    display: flex;
    align-items: center;
    gap: 8px;
    color: #8a96ac;
    font-size: 13px;
  }
  .status code { color: #cdd6e3; }
  .status-text {
    text-transform: uppercase;
    font-size: 11px;
    letter-spacing: 0.08em;
  }
  .dot {
    width: 9px; height: 9px;
    border-radius: 50%;
    background: #555;
  }
  .dot.open { background: #2fd07a; box-shadow: 0 0 8px #2fd07a88; }
  .dot.connecting { background: #f0c84a; }
  .dot.error { background: #e85d6d; }
  .dot.disconnected { background: #555; }

  .stats {
    display: flex;
    gap: 12px;
    margin-bottom: 20px;
    flex-wrap: wrap;
  }
  .stats > div {
    background: #111827;
    border: 1px solid #1f2a3d;
    padding: 10px 14px;
    border-radius: 6px;
    min-width: 160px;
  }
  .stats label {
    display: block;
    color: #8a96ac;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    margin-bottom: 4px;
  }
  .stats b {
    font-size: 22px;
    font-weight: 600;
    color: #e7ecf3;
  }

  .grid {
    display: grid;
    grid-template-columns: minmax(280px, 1fr) 2fr;
    gap: 18px;
  }
  @media (max-width: 900px) {
    .grid { grid-template-columns: 1fr; }
  }

  .card {
    background: #0f1626;
    border: 1px solid #1f2a3d;
    border-radius: 8px;
    padding: 16px 18px;
  }
  .card h2 {
    margin: 0 0 12px;
    font-size: 14px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: #b1bdcf;
    font-weight: 600;
  }
  .muted { color: #6c7891; font-weight: normal; }

  table {
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
  }
  thead th {
    color: #6c7891;
    font-weight: 500;
    text-align: left;
    padding: 6px 8px;
    border-bottom: 1px solid #1f2a3d;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
  }
  tbody td {
    padding: 6px 8px;
    border-bottom: 1px solid #18223730;
  }
  tbody tr:hover { background: #131d33; }
  .num { text-align: right; font-variant-numeric: tabular-nums; }
  .bid { color: #5cd2a4; }
  .ask { color: #f08080; }
  .strong { color: #e7ecf3; font-weight: 600; }

  .empty {
    color: #6c7891;
    font-style: italic;
    margin: 8px 0;
  }

  footer {
    margin-top: 28px;
    padding-top: 18px;
    border-top: 1px solid #1a2333;
    color: #8a96ac;
    font-size: 13px;
  }
  footer code {
    background: #111827;
    border: 1px solid #1f2a3d;
    padding: 1px 6px;
    border-radius: 3px;
  }
</style>