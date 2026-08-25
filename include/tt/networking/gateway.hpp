// include/tt/networking/gateway.hpp
//
// Tiny TCP gateway. Listens on a port, spawns a thread per connection,
// parses line-based commands, dispatches to the matching engine, and
// pushes replies back to the client. The matching engine runs on the
// same thread (single-connection mode) or shared (multi-connection mode).
//
// This implementation is single-threaded inside one connection at a time:
// the server thread accepts and spawns a per-client thread. Each client
// thread drives the matching engine synchronously. Good enough for the
// project's scope; production would use epoll/io_uring and the worker
// pool from Phase 9.

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/market_data/publisher.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace tt {

class Gateway {
public:
    Gateway(MatchingEngine& engine, MarketDataPublisher& publisher,
            std::uint16_t port);
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // Bind & listen. Returns true on success.
    bool start();

    // Stop accepting new connections and shut down all client threads.
    void stop();

    // For tests: process a single buffer in-memory and return the
    // responses as a single string. No socket I/O. Useful for unit
    // testing the protocol without spinning up a server.
    static std::string process_buffer(const std::string& in,
                                      MatchingEngine& engine,
                                      MarketDataPublisher& publisher);

private:
    void accept_loop();
    void client_loop(int fd);

    MatchingEngine&      engine_;
    MarketDataPublisher& publisher_;
    std::uint16_t        port_;
    int                  listen_fd_{-1};
    std::atomic<bool>    running_{false};
    std::vector<std::thread> client_threads_;
};

}  // namespace tt