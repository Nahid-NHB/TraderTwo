// src/networking/gateway.cpp

#include "tt/networking/gateway.hpp"
#include "tt/networking/protocol.hpp"
#include "tt/orderbook/order_book.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace tt {

namespace {

class GatewaySink final : public TradeSink {
public:
    GatewaySink(MarketDataPublisher& pub, std::vector<std::string>* log)
        : pub_(pub), log_(log) {}
    void on_trade(const Trade& t) noexcept override {
        if (log_) log_->push_back(format_trade(t));
        pub_.on_trade(t);
    }
    void on_submit_result(const SubmitResult& r) noexcept override {
        if (log_) {
            std::string status;
            switch (r.status) {
                case SubmitStatus::Accepted:        status = "ACCEPTED"; break;
                case SubmitStatus::FullyFilled:     status = "FILLED";   break;
                case SubmitStatus::PartiallyFilled: status = "PARTIAL";  break;
                case SubmitStatus::Cancelled:       status = "CANCELLED";break;
                case SubmitStatus::Rejected:        status = "REJECTED"; break;
            }
            log_->push_back(format_submit_reply(r.order_id, r.sequence, status));
        }
        pub_.on_submit_result(r);
    }
private:
    MarketDataPublisher& pub_;
    std::vector<std::string>* log_;
};

int read_line(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n == 0) return 0;          // EOF
        if (n < 0)  return -1;
        if (c == '\n') return 1;
        if (c == '\r') continue;
        out.push_back(c);
        if (out.size() > 4096) return -1;  // line too long
    }
}

ssize_t write_all(int fd, const std::string& s) {
    std::size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
        if (n < 0) return n;
        if (n == 0) break;
        sent += static_cast<std::size_t>(n);
    }
    return static_cast<ssize_t>(sent);
}

}  // namespace

std::string Gateway::process_buffer(const std::string& in,
                                    MatchingEngine& engine,
                                    MarketDataPublisher& publisher) {
    std::string out;
    std::vector<std::string> log;
    GatewaySink sink(publisher, &log);

    std::size_t pos = 0;
    while (pos < in.size()) {
        std::size_t eol = in.find('\n', pos);
        if (eol == std::string::npos) break;
        std::string_view line(in.data() + pos, eol - pos);
        pos = eol + 1;
        ParsedRequest req;
        auto err = parse_request(line, req);
        if (!err.ok) {
            out += format_error(err.message);
            continue;
        }
        switch (req.command) {
            case InboundCommand::Ping: {
                out += "PONG\n";
                break;
            }
            case InboundCommand::Quote: {
                OrderBook* ob = engine.book(req.instrument);
                if (!ob) { out += format_error("instrument not registered"); break; }
                auto b = ob->top_bid();
                auto a = ob->top_ask();
                out += format_quote(req.instrument,
                                    b.valid ? b.price : Price{-1},
                                    b.valid ? b.quantity : Quantity{0},
                                    a.valid ? a.price : Price{-1},
                                    a.valid ? a.quantity : Quantity{0});
                break;
            }
            case InboundCommand::New: {
                auto o = std::make_unique<Order>();
                o->trader_id     = req.trader_id;
                o->instrument_id = req.instrument;
                o->side          = req.side;
                o->type          = req.type;
                o->tif           = req.tif;
                o->price         = req.price;
                o->quantity      = req.qty;
                o->remaining     = req.qty;
                engine.submit(std::move(o), sink);
                break;
            }
            case InboundCommand::Cancel: {
                engine.cancel(req.instrument, req.order_id);
                break;
            }
            case InboundCommand::Modify: {
                auto r = engine.modify(req.instrument, req.order_id, req.qty, req.price);
                (void)r;
                break;
            }
            default:
                out += format_error("unknown command");
                break;
        }
    }
    for (const auto& l : log) out += l;
    return out;
}

Gateway::Gateway(MatchingEngine& engine, MarketDataPublisher& publisher,
                 std::uint16_t port)
    : engine_(engine), publisher_(publisher), port_(port) {}

Gateway::~Gateway() { stop(); }

bool Gateway::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    if (::listen(listen_fd_, 16) < 0) {
        ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    running_ = true;
    client_threads_.emplace_back([this]{ accept_loop(); });
    return true;
}

void Gateway::stop() {
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    running_ = false;
    for (auto& t : client_threads_) {
        if (t.joinable()) t.join();
    }
    client_threads_.clear();
}

void Gateway::accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cfd < 0) {
            if (!running_.load()) break;
            continue;
        }
        client_threads_.emplace_back([this, cfd]{ client_loop(cfd); });
    }
}

void Gateway::client_loop(int fd) {
    std::vector<std::string> log;
    GatewaySink sink(publisher_, &log);
    std::string line;
    char buf[1024];
    while (running_.load(std::memory_order_acquire)) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        std::string chunk(buf, static_cast<std::size_t>(n));
        std::size_t pos = 0;
        while (pos < chunk.size()) {
            std::size_t eol = chunk.find('\n', pos);
            if (eol == std::string::npos) {
                line.append(chunk, pos, chunk.size() - pos);
                pos = chunk.size();
            } else {
                line.append(chunk, pos, eol - pos);
                pos = eol + 1;
                ParsedRequest req;
                auto err = parse_request(line, req);
                if (!err.ok) {
                    write_all(fd, format_error(err.message));
                } else {
                    switch (req.command) {
                        case InboundCommand::Ping:
                            write_all(fd, "PONG\n");
                            break;
                        case InboundCommand::Quote: {
                            OrderBook* ob = engine_.book(req.instrument);
                            if (!ob) {
                                write_all(fd, format_error("instrument not registered"));
                            } else {
                                auto b = ob->top_bid();
                                auto a = ob->top_ask();
                                write_all(fd, format_quote(req.instrument,
                                    b.valid ? b.price : Price{-1},
                                    b.valid ? b.quantity : Quantity{0},
                                    a.valid ? a.price : Price{-1},
                                    a.valid ? a.quantity : Quantity{0}));
                            }
                            break;
                        }
                        case InboundCommand::New: {
                            auto o = std::make_unique<Order>();
                            o->trader_id     = req.trader_id;
                            o->instrument_id = req.instrument;
                            o->side          = req.side;
                            o->type          = req.type;
                            o->tif           = req.tif;
                            o->price         = req.price;
                            o->quantity      = req.qty;
                            o->remaining     = req.qty;
                            engine_.submit(std::move(o), sink);
                            break;
                        }
                        case InboundCommand::Cancel:
                            engine_.cancel(req.instrument, req.order_id);
                            break;
                        case InboundCommand::Modify:
                            engine_.modify(req.instrument, req.order_id, req.qty, req.price);
                            break;
                        default:
                            write_all(fd, format_error("unknown command"));
                            break;
                    }
                }
                line.clear();
            }
        }
    }
    for (const auto& l : log) write_all(fd, l);
    ::close(fd);
}

}  // namespace tt