// src/lib/marketData.js
//
// Tiny WebSocket client + Svelte store. The C++ matching engine broadcasts
// JSON messages of two shapes:
//
//   trade:  {"type":"trade","i":<instrument>,"buy":<id>,"sell":<id>,
//                 "px":<price>,"qty":<qty>,"seq":<sequence>}
//   tob:    {"type":"tob","i":<instrument>,"b":<price>,"bq":<qty>,
//                 "a":<price>,"aq":<qty>,"hb":<0|1>,"ha":<0|1>}
//
// The store returns three things:
//   - events$:  log of all raw messages (capped at MAX_LOG)
//   - trades$:  derived list of trades (most recent first)
//   - books$:   derived map of instrument -> TopOfBook
// plus a connection status string for the UI.

import { writable } from 'svelte/store';

const MAX_LOG = 1024;

function createMarketDataStore(url) {
  const status = writable('disconnected'); // disconnected | connecting | open | error
  const events = writable([]);             // raw messages, newest at end
  const socket = writable(null);

  function connect() {
    status.set('connecting');
    let ws;
    try {
      ws = new WebSocket(url);
    } catch (e) {
      status.set('error');
      return;
    }
    socket.set(ws);

    ws.addEventListener('open', () => status.set('open'));
    ws.addEventListener('error', () => status.set('error'));
    ws.addEventListener('close', () => status.set('disconnected'));

    ws.addEventListener('message', (ev) => {
      let parsed;
      try { parsed = JSON.parse(ev.data); }
      catch { return; }
      events.update((list) => {
        const next = list.length >= MAX_LOG
          ? list.slice(list.length - MAX_LOG + 1)
          : list.slice();
        next.push({ ts: Date.now(), ...parsed });
        return next;
      });
    });
  }

  function disconnect() {
    socket.update((ws) => {
      if (ws && ws.readyState <= WebSocket.OPEN) ws.close();
      return null;
    });
  }

  return { status, events, socket, connect, disconnect };
}

// Derived stores are built in the page component since the data shape
// (top-of-book + trade) is page-specific. The store returned below is a
// thin wrapper around the raw WebSocket connection.

export { createMarketDataStore, MAX_LOG };