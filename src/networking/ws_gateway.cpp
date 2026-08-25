// src/networking/ws_gateway.cpp
//
// Implementation of WsGateway. Each command handler takes the engine
// mutex, dispatches synchronously, and replies to the originating peer
// with a JSON message tagged with the original request id (if any).
//
// Trade events from WS-submitted orders are forwarded to the
// MarketDataPublisher so the WS server (which subscribes to the publisher)
// broadcasts them to every connected client, not just the submitting peer.

#include "tt/networking/ws_gateway.hpp"

#include <cstdio>
#include <string>

namespace tt {

WsGateway::WsGateway(MatchingEngine& engine, MarketDataPublisher& pub,
                     WsServer& ws)
    : engine_(engine), pub_(pub), ws_(ws) {}

void WsGateway::install() {
    WsCommandCallbacks cbs;
    cbs.on_submit = [this](WsPeer& p, const std::string& r,
                          std::uint64_t i, std::uint64_t t, int s,
                          std::int64_t px, std::int64_t qty,
                          const std::string& tif) {
        on_submit(p, r, i, t, s, px, qty, tif);
    };
    cbs.on_cancel = [this](WsPeer& p, const std::string& r,
                          std::uint64_t i, std::uint64_t oid) {
        on_cancel(p, r, i, oid);
    };
    cbs.on_modify = [this](WsPeer& p, const std::string& r,
                          std::uint64_t i, std::uint64_t oid,
                          std::int64_t qty, std::int64_t px) {
        on_modify(p, r, i, oid, qty, px);
    };
    cbs.on_ping   = [this](WsPeer& p, const std::string& r) {
        on_ping(p, r);
    };
    ws_.set_command_callbacks(std::move(cbs));
}

std::string WsGateway::status_name(SubmitStatus s) {
    switch (s) {
        case SubmitStatus::Accepted:        return "ACCEPTED";
        case SubmitStatus::FullyFilled:     return "FILLED";
        case SubmitStatus::PartiallyFilled: return "PARTIAL";
        case SubmitStatus::Cancelled:       return "CANCELLED";
        case SubmitStatus::Rejected:        return "REJECTED";
    }
    return "UNKNOWN";
}

void WsGateway::on_submit(WsPeer& peer, const std::string& req,
                          std::uint64_t instrument, std::uint64_t trader,
                          int side, std::int64_t px, std::int64_t qty,
                          const std::string& tif) {
    // Sink that does two things:
    //   1) Forwards on_trade / on_submit_result to the MarketDataPublisher so
    //      trade and TOB broadcasts still happen when a submission comes in
    //      via this WS gateway.
    //   2) Captures on_submit_result and replies on the originating peer.
    // The publisher already feeds the WS server's listener, so trades fan
    // out to all connected WS clients.
    struct PerPeerSink final : public TradeSink {
        MarketDataPublisher* pub{nullptr};
        WsPeer* peer{nullptr};
        std::string req;
        void on_trade(const Trade& t) noexcept override {
            if (pub) pub->on_trade(t);
        }
        void on_submit_result(const SubmitResult& r) noexcept override {
            if (pub) pub->on_submit_result(r);
            char buf[512];
            if (r.status == SubmitStatus::Rejected) {
                std::snprintf(buf, sizeof(buf),
                    R"({"type":"submit_result","req":"%s","status":"REJECTED","reason":"%s"})",
                    req.c_str(), r.reject_reason.c_str());
            } else {
                std::snprintf(buf, sizeof(buf),
                    R"({"type":"submit_result","req":"%s","status":"%s","id":%lu,"i":%lu,"seq":%lu,"filled":%lld,"resting":%lld})",
                    req.c_str(),
                    r.status == SubmitStatus::Accepted       ? "ACCEPTED" :
                    r.status == SubmitStatus::FullyFilled    ? "FILLED" :
                    r.status == SubmitStatus::PartiallyFilled? "PARTIAL" :
                                                                "CANCELLED",
                    static_cast<unsigned long>(r.order_id),
                    static_cast<unsigned long>(r.instrument_id),
                    static_cast<unsigned long>(r.sequence),
                    static_cast<long long>(r.filled_quantity.qty),
                    static_cast<long long>(r.resting_quantity.qty));
            }
            if (peer && peer->open()) peer->send_text(buf);
        }
    } sink;
    sink.pub  = &pub_;
    sink.peer = &peer;
    sink.req  = req;

    Side s = (side == 0) ? Side::Buy : Side::Sell;
    TimeInForce t = TimeInForce::GTC;
    if (tif == "IOC") t = TimeInForce::IOC;
    else if (tif == "FOK") t = TimeInForce::FOK;

    std::lock_guard<std::mutex> lk(engine_mtx_);
    try {
        engine_.submit_limit(InstrumentId{instrument}, TraderId{trader}, s,
                             Price{px}, Quantity{qty}, t, sink);
    } catch (...) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            R"({"type":"submit_result","req":"%s","status":"REJECTED","reason":"engine exception"})",
            req.c_str());
        peer.send_text(buf);
    }
}

void WsGateway::on_cancel(WsPeer& peer, const std::string& req,
                          std::uint64_t instrument, std::uint64_t order_id) {
    bool ok;
    {
        std::lock_guard<std::mutex> lk(engine_mtx_);
        ok = engine_.cancel(InstrumentId{instrument}, OrderId{order_id});
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"cancel_result","req":"%s","ok":%s,"i":%lu,"id":%lu})",
        req.c_str(), ok ? "true" : "false",
        static_cast<unsigned long>(instrument),
        static_cast<unsigned long>(order_id));
    peer.send_text(buf);
}

void WsGateway::on_modify(WsPeer& peer, const std::string& req,
                          std::uint64_t instrument, std::uint64_t order_id,
                          std::int64_t qty, std::int64_t px) {
    OrderBook::ModifyResult r;
    {
        std::lock_guard<std::mutex> lk(engine_mtx_);
        r = engine_.modify(InstrumentId{instrument}, OrderId{order_id},
                           Quantity{qty}, Price{px});
    }
    const char* status_str = "UNKNOWN";
    switch (r) {
        case OrderBook::ModifyResult::Modified: status_str = "MODIFIED"; break;
        case OrderBook::ModifyResult::Replaced: status_str = "REPLACED"; break;
        case OrderBook::ModifyResult::NotFound: status_str = "NOT_FOUND"; break;
        case OrderBook::ModifyResult::Rejected: status_str = "REJECTED"; break;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"modify_result","req":"%s","status":"%s","i":%lu,"id":%lu})",
        req.c_str(), status_str,
        static_cast<unsigned long>(instrument),
        static_cast<unsigned long>(order_id));
    peer.send_text(buf);
}

void WsGateway::on_ping(WsPeer& peer, const std::string& req) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), R"({"type":"pong","req":"%s"})", req.c_str());
    peer.send_text(buf);
}

}  // namespace tt
