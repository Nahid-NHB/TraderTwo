// src/lib/orderEntry.js
//
// Client-side wrappers around the WS gateway's typed-command protocol.
//
// The C++ server understands four commands (issued as JSON text frames):
//   {type:"ping",   req:"..."}
//   {type:"submit", req:"...", i, trader, side, px, qty, tif}
//   {type:"cancel", req:"...", i, id}
//   {type:"modify", req:"...", i, id, qty, px}
//
// Replies come back on the same connection as well, with type:"pong",
// "submit_result", "cancel_result" or "modify_result". We expose them as
// a Svelte store so the UI can show acceptance / rejection inline.

import { writable } from 'svelte/store';

const MAX_REPLIES = 200;

/**
 * Attach command/event hooks to a WebSocket. Returns {sendCommand, replies,
 * socket} so the UI can both send and observe.
 */
function attach(ws) {
  const replies = writable([]);

  ws.addEventListener('message', (ev) => {
    let parsed;
    try { parsed = JSON.parse(ev.data); } catch { return; }
    // Only handle the reply / event types we care about. Trades + TOBs are
    // already consumed by the market-data store.
    const t = parsed?.type;
    if (t === 'pong' || t === 'submit_result' || t === 'cancel_result' ||
        t === 'modify_result') {
      replies.update((list) => {
        const next = list.length >= MAX_REPLIES
          ? list.slice(list.length - MAX_REPLIES + 1)
          : list.slice();
        next.push({ ts: Date.now(), ...parsed });
        return next;
      });
    }
  });

  function sendCommand(obj) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return false;
    ws.send(JSON.stringify(obj));
    return true;
  }

  return { sendCommand, replies };
}

/**
 * Create a store-fronted order-entry controller. The caller supplies the live
 * WebSocket; we install reply listeners and provide a sendCommand helper.
 */
function createOrderEntry(ws) {
  const internal = attach(ws);
  return { sendCommand: internal.sendCommand, replies: internal.replies };
}

export { createOrderEntry };
