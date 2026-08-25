// include/tt/networking/ws_server.hpp
//
// Minimal RFC 6455 WebSocket server built directly on BSD sockets. Handshake
// is HTTP/1.1 Upgrade; frames are parsed in place; text/binary frames are
// surfaced as std::string callbacks. Pings get answered with pongs; close
// frames are acknowledged.
//
// We deliberately implement only what the dashboard needs:
//   - server-side handshake
//   - text & binary frames from client to server
//   - text & binary frames from server to client
//   - ping/pong
//   - close
//
// No fragmentation, no per-frame compression. Each client runs on its own
// thread.

#pragma once

#include "tt/market_data/events.hpp"
#include "tt/market_data/publisher.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace tt {

// One connected WebSocket peer.
class WsPeer {
public:
    WsPeer(int fd, uint64_t id);
    ~WsPeer();

    uint64_t id() const noexcept { return id_; }
    int      fd()  const noexcept { return fd_; }
    bool     open() const noexcept { return open_; }

    // Send a text frame. Returns false on socket error.
    bool send_text(const std::string& payload);

    // Close gracefully with the given status code (default 1000 = normal).
    void close(uint16_t code = 1000);

private:
    friend class WsServer;
    void close_internal();

    int      fd_;
    uint64_t id_;
    std::atomic<bool> open_{true};
};

// Internal: shared handle to a peer. WsServer holds the canonical reference
// in its map; serve_peer and broadcast snapshots carry a copy so the peer
// stays alive even if the map entry is erased concurrently.
using WsPeerPtr = std::shared_ptr<WsPeer>;

// Per-peer callback bundle. Wired up by the application.
struct WsPeerCallbacks {
    std::function<void(WsPeer&, const std::string& text)> on_text;
    std::function<void(WsPeer&, const std::string& bin)>  on_binary;
    std::function<void(WsPeer&)>                          on_close;
};

// Multi-client WebSocket server. Lives alongside the TCP gateway; the
// gateway process drives matching on its thread and the WS server forwards
// market-data updates on a separate thread.
class WsServer {
public:
    WsServer(MarketDataPublisher& pub, std::uint16_t port);
    ~WsServer();

    WsServer(const WsServer&)            = delete;
    WsServer& operator=(const WsServer&) = delete;

    bool start();
    void stop();

    std::uint16_t port() const noexcept { return port_; }
    bool          running() const noexcept { return running_.load(std::memory_order_acquire); }

    // Set per-peer callbacks. Callbacks run on the WS server thread for
    // incoming text/binary frames; market-data updates fan out from the
    // publisher thread (see PublisherListenerAdapter below).
    void set_callbacks(WsPeerCallbacks cbs) { cbs_ = std::move(cbs); }

    // Broadcast a text frame to every connected peer. Lock-free during the
    // broadcast loop via the peer_set_ mutex.
    void broadcast_text(const std::string& payload);

private:
    void accept_loop();
    void serve_peer(uint64_t id, int fd, WsPeerPtr peer);
    void on_publisher_event(const Event& e);
    void on_publisher_tob(InstrumentId id);

    MarketDataPublisher& pub_;
    std::uint16_t       port_;
    int                 listen_fd_{-1};
    std::atomic<bool>   running_{false};
    std::vector<std::thread>      threads_;
    std::unordered_set<uint64_t>  peer_ids_;
    std::mutex                    peer_ids_mtx_;
    std::unordered_map<uint64_t, WsPeerPtr> peers_;
    std::mutex                    peers_mtx_;
    WsPeerCallbacks cbs_;

    // Publisher listener adapter — translates MarketDataListener callbacks
    // into WS broadcasts. We store the listener object directly and register
    // it with the publisher on start().
    struct Listener;
    std::unique_ptr<Listener> listener_;
};

// Helpers used by tests; public so the in-process client can reuse them.
// Returns the number of header bytes consumed, or 0 on failure.
std::size_t ws_encode_server_frame(std::string& out,
                                   bool text,
                                   const std::string& payload);

}  // namespace tt