// include/tt/networking/ws_gateway.hpp
//
// Glue between WsServer and MatchingEngine. Wires the four command
// callbacks (submit / cancel / modify / ping) so a connected WS client
// can drive the engine directly. Replies are routed back to the
// originating peer only.
//
// The matching engine is single-threaded per the docs, so all command
// handlers funnel through a single mutex. This is a deliberate
// simplification — production would use the Phase-9 worker pool to
// parallelise per instrument.

#pragma once

#include "tt/market_data/publisher.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/networking/ws_server.hpp"

#include <memory>
#include <mutex>

namespace tt {

class WsGateway {
public:
    // WsGateway forwards trade events to the publisher so that WS
    // broadcasts still happen when an aggressor submits via this gateway.
    WsGateway(MatchingEngine& engine, MarketDataPublisher& pub, WsServer& ws);

    // Install the command callbacks on the WS server. Idempotent.
    void install();

private:
    void on_submit(WsPeer& peer, const std::string& req,
                   std::uint64_t instrument, std::uint64_t trader,
                   int side, std::int64_t px, std::int64_t qty,
                   const std::string& tif);
    void on_cancel(WsPeer& peer, const std::string& req,
                   std::uint64_t instrument, std::uint64_t order_id);
    void on_modify(WsPeer& peer, const std::string& req,
                   std::uint64_t instrument, std::uint64_t order_id,
                   std::int64_t qty, std::int64_t px);
    void on_ping(WsPeer& peer, const std::string& req);

    static std::string status_name(SubmitStatus s);

    MatchingEngine&       engine_;
    MarketDataPublisher&  pub_;
    WsServer&             ws_;
    std::mutex            engine_mtx_;
};

}  // namespace tt
