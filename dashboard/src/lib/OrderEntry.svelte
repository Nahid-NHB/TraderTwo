<script>
  import { derived } from 'svelte/store';

  // Reactive props / stores.
  export let instruments = [];
  export let orderEntry;
  export let selectedInstrument = 1;

  // Side, TIF, mode (submit / cancel / modify). Mode drives which fields
  // are visible and which JSON keys we send.
  let mode = 'submit';             // 'submit' | 'cancel' | 'modify'
  let side = 'buy';                // 'buy' | 'sell'
  let tif = 'GTC';                 // 'GTC' | 'IOC' | 'FOK'
  let instrId = selectedInstrument;
  let trader = 1;
  let price = 100;
  let qty = 10;
  let orderId = '';
  let busy = false;
  let lastSent = null;
  let err = '';

  $: replies = orderEntry?.replies || { subscribe: () => () => {} };

  // Pull the most recent reply matching our mode / req-id.
  const latestReply = derived(replies, ($r) => $r.length ? $r[$r.length - 1] : null);

  function sendSubmit() {
    err = '';
    if (!orderEntry?.sendCommand) { err = 'WS not connected'; return; }
    const req = 'sub_' + Date.now();
    const ok = orderEntry.sendCommand({
      type: 'submit',
      req,
      i: Number(instrId),
      trader: Number(trader),
      side: side === 'buy' ? 0 : 1,
      px: Number(price),
      qty: Number(qty),
      tif
    });
    if (!ok) { err = 'WS not open'; return; }
    busy = true;
    lastSent = { type: 'submit', req, i: instrId, px: price, qty };
  }

  function sendCancel() {
    err = '';
    if (!orderEntry?.sendCommand) { err = 'WS not connected'; return; }
    const req = 'cnl_' + Date.now();
    const id = Number(orderId);
    if (!Number.isFinite(id) || id <= 0) { err = 'order id required'; return; }
    const ok = orderEntry.sendCommand({
      type: 'cancel', req,
      i: Number(instrId),
      id
    });
    if (!ok) { err = 'WS not open'; return; }
    lastSent = { type: 'cancel', req, i: instrId, id };
  }

  function sendModify() {
    err = '';
    if (!orderEntry?.sendCommand) { err = 'WS not connected'; return; }
    const req = 'mod_' + Date.now();
    const id = Number(orderId);
    if (!Number.isFinite(id) || id <= 0) { err = 'order id required'; return; }
    const ok = orderEntry.sendCommand({
      type: 'modify', req,
      i: Number(instrId),
      id,
      qty: Number(qty),
      px: Number(price)
    });
    if (!ok) { err = 'WS not open'; return; }
    lastSent = { type: 'modify', req, i: instrId, id, qty, px: price };
  }

  function sendPing() {
    err = '';
    if (!orderEntry?.sendCommand) { err = 'WS not connected'; return; }
    const req = 'ping_' + Date.now();
    const ok = orderEntry.sendCommand({ type: 'ping', req });
    if (!ok) { err = 'WS not open'; return; }
    lastSent = { type: 'ping', req };
  }

  function submit() {
    if (mode === 'submit') return sendSubmit();
    if (mode === 'cancel') return sendCancel();
    if (mode === 'modify') return sendModify();
  }

  function statusClass(t) {
    if (!t) return '';
    if (t === 'ACCEPTED' || t === 'FILLED' || t === 'MODIFIED' || t === 'REPLACED' || t === 'pong') return 'ok';
    if (t === 'REJECTED' || t === 'NOT_FOUND') return 'err';
    if (t === 'PARTIAL' || t === 'CANCELLED') return 'mid';
    return '';
  }
</script>

<div class="card order-entry">
  <div class="card-head">
    <h2>Order entry</h2>
    <span class="muted">{$latestReply ? `last: ${$latestReply.type}` : 'no reply yet'}</span>
  </div>

  <div class="modes">
    {#each ['submit', 'cancel', 'modify'] as m}
      <button class:active={mode === m} on:click={() => mode = m}>{m}</button>
    {/each}
  </div>

  <div class="grid">
    <label>
      <span>Instrument</span>
      <select bind:value={instrId}>
        {#each instruments as i}
          <option value={i}>{i}</option>
        {/each}
      </select>
    </label>
    {#if mode === 'submit'}
      <label>
        <span>Trader</span>
        <input type="number" min="1" bind:value={trader} />
      </label>
    {/if}
    {#if mode === 'submit' || mode === 'modify'}
      <label>
        <span>Side</span>
        <div class="seg">
          <button class:active={side === 'buy'}  on:click={() => side = 'buy'}>Buy</button>
          <button class:active={side === 'sell'} on:click={() => side = 'sell'}>Sell</button>
        </div>
      </label>
      <label>
        <span>Price</span>
        <input type="number" bind:value={price} />
      </label>
      <label>
        <span>Qty</span>
        <input type="number" min="1" bind:value={qty} />
      </label>
      {#if mode === 'submit'}
        <label>
          <span>TIF</span>
          <select bind:value={tif}>
            <option value="GTC">GTC</option>
            <option value="IOC">IOC</option>
            <option value="FOK">FOK</option>
          </select>
        </label>
      {/if}
    {/if}
    {#if mode === 'cancel' || mode === 'modify'}
      <label>
        <span>Order id</span>
        <input type="number" min="1" bind:value={orderId} placeholder="e.g. 1" />
      </label>
    {/if}
  </div>

  <div class="actions">
    <button class="primary" disabled={busy} on:click={submit}>
      {mode === 'submit' ? 'Submit' : mode === 'cancel' ? 'Cancel' : 'Modify'}
    </button>
    <button class="ghost" disabled={busy} on:click={sendPing}>Ping</button>
    {#if err}<span class="err">{err}</span>{/if}
  </div>

  {#if $latestReply}
    <pre class="reply {statusClass($latestReply.status ?? $latestReply.type)}">{JSON.stringify($latestReply, null, 2)}</pre>
  {/if}
  {#if lastSent}
    <p class="sent">→ sent {$latestReply ? '' : '(awaiting reply)'}</p>
  {/if}
</div>

<style>
  .order-entry { display: flex; flex-direction: column; gap: 10px; }
  .modes { display: flex; gap: 6px; }
  .modes button {
    background: #111827;
    color: #8a96ac;
    border: 1px solid #1f2a3d;
    border-radius: 4px;
    padding: 4px 12px;
    font-size: 12px;
    cursor: pointer;
    text-transform: capitalize;
  }
  .modes button.active { background: #1a3a6c; color: #e7ecf3; border-color: #2a4a8e; }

  .grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 8px;
  }
  label { display: flex; flex-direction: column; gap: 4px; font-size: 11px; }
  label > span { color: #8a96ac; text-transform: uppercase; letter-spacing: 0.06em; }
  input, select {
    background: #111827;
    color: #e7ecf3;
    border: 1px solid #1f2a3d;
    border-radius: 4px;
    padding: 5px 8px;
    font-size: 13px;
    font-family: inherit;
    font-variant-numeric: tabular-nums;
  }

  .seg { display: flex; gap: 4px; }
  .seg button {
    flex: 1;
    background: #111827;
    color: #8a96ac;
    border: 1px solid #1f2a3d;
    border-radius: 4px;
    padding: 5px;
    font-size: 12px;
    cursor: pointer;
  }
  .seg button.active { background: #14360e; color: #5cd2a4; border-color: #2a6e2a; }

  .actions { display: flex; align-items: center; gap: 8px; }
  .actions button.primary {
    flex: 1;
    background: linear-gradient(135deg, #19305c 0%, #2a4a8e 100%);
    color: #e7ecf3;
    border: 1px solid #2a4a8e;
    padding: 7px;
    border-radius: 4px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
  }
  .actions button.primary:disabled { opacity: 0.5; cursor: default; }
  .actions button.ghost {
    background: transparent;
    color: #8a96ac;
    border: 1px solid #1f2a3d;
    padding: 7px 12px;
    border-radius: 4px;
    font-size: 12px;
    cursor: pointer;
  }
  .actions .err { color: #f08080; font-size: 12px; }

  pre.reply {
    margin: 0;
    background: #0a111e;
    border: 1px solid #1f2a3d;
    border-radius: 4px;
    padding: 8px 10px;
    font-size: 12px;
    color: #cdd6e3;
    max-height: 160px;
    overflow: auto;
  }
  pre.reply.ok  { border-color: #2a6e2a; color: #5cd2a4; }
  pre.reply.err { border-color: #6e2a2a; color: #f08080; }
  pre.reply.mid { border-color: #6e5e2a; color: #e5c66f; }

  .sent { margin: 0; color: #6c7891; font-size: 11px; }
  .err  { color: #f08080; }
</style>
