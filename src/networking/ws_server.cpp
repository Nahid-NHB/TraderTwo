// src/networking/ws_server.cpp
//
// Minimal RFC 6455 WebSocket server. Parses client frames in place, hashes
// the Sec-WebSocket-Key during handshake with SHA-1, and writes server
// frames with a 2-byte length prefix (or 8-byte extended length for large
// payloads). 99.9% of frames in this project are < 1 KiB so the simple path
// is the common one.

#include "tt/networking/ws_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <string_view>

namespace tt {

namespace {

// RFC 6455 magic GUID used during the handshake hash.
constexpr std::string_view kWsMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Base64 encode. Standard alphabet, no line breaks. Caller owns the buffer.
std::string base64_encode(const unsigned char* data, std::size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        unsigned int a = data[i];
        unsigned int b = (i + 1 < len) ? data[i + 1] : 0;
        unsigned int c = (i + 2 < len) ? data[i + 2] : 0;
        unsigned int v = (a << 16) | (b << 8) | c;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? tbl[v & 0x3F]        : '=');
    }
    return out;
}

std::string sha1_b64(std::string_view in) {
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(in.data()), in.size(), digest);
    return base64_encode(digest, SHA_DIGEST_LENGTH);
}

// Case-insensitive header lookup. Returns the value after the colon, trimmed.
std::string find_header(const std::string& req, std::string_view name) {
    std::string needle;
    needle.reserve(name.size());
    for (char c : name) needle.push_back(static_cast<char>(std::tolower(c)));
    std::string hay;
    hay.reserve(req.size());
    for (char c : req) hay.push_back(static_cast<char>(std::tolower(c)));
    auto pos = hay.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    if (pos >= hay.size() || hay[pos] != ':') return {};
    ++pos;
    while (pos < hay.size() && (hay[pos] == ' ' || hay[pos] == '\t')) ++pos;
    auto end = hay.find('\n', pos);
    if (end == std::string::npos) end = hay.size();
    while (end > pos && (hay[end - 1] == '\r' || hay[end - 1] == ' '
                        || hay[end - 1] == '\t')) --end;
    return req.substr(pos, end - pos);
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

ssize_t write_all(int fd, const void* data, std::size_t n) {
    const char* p = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < n) {
        ssize_t k = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (k <= 0) return k;
        sent += static_cast<std::size_t>(k);
    }
    return static_cast<ssize_t>(sent);
}

ssize_t read_some(int fd, void* buf, std::size_t cap) {
    return ::recv(fd, buf, cap, 0);
}

bool send_all(int fd, const std::string& s) {
    return write_all(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size());
}

bool parse_request_line(const std::string& req,
                        std::string& method,
                        std::string& path,
                        std::string& version) {
    auto eol = req.find('\n');
    if (eol == std::string::npos) return false;
    std::string line = req.substr(0, eol);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::size_t p1 = line.find(' ');
    if (p1 == std::string::npos) return false;
    std::size_t p2 = line.find(' ', p1 + 1);
    if (p2 == std::string::npos) return false;
    method  = line.substr(0, p1);
    path    = line.substr(p1 + 1, p2 - p1 - 1);
    version = line.substr(p2 + 1);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame encoding helper (also exposed in the header for tests).
// ---------------------------------------------------------------------------
std::size_t ws_encode_server_frame(std::string& out,
                                   bool text,
                                   const std::string& payload) {
    std::size_t start = out.size();
    std::uint8_t b0 = static_cast<std::uint8_t>(0x80 | (text ? 0x1 : 0x2));
    out.push_back(static_cast<char>(b0));
    std::uint64_t len = payload.size();
    if (len <= 125) {
        out.push_back(static_cast<char>(len));  // MASK bit clear; server→client un-masked
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
    out.append(payload);
    return out.size() - start;
}

// ---------------------------------------------------------------------------
// WsPeer
// ---------------------------------------------------------------------------
WsPeer::WsPeer(int fd, uint64_t id) : fd_(fd), id_(id) {}
WsPeer::~WsPeer() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool WsPeer::send_text(const std::string& payload) {
    if (!open_.load(std::memory_order_acquire)) return false;
    std::string buf;
    ws_encode_server_frame(buf, /*text=*/true, payload);
    return send_all(fd_, buf);
}

void WsPeer::close(uint16_t code) {
    std::string body;
    body.push_back(static_cast<char>((code >> 8) & 0xFF));
    body.push_back(static_cast<char>(code & 0xFF));
    std::string frame;
    ws_encode_server_frame(frame, /*text=*/false, body);  // close = opcode 8
    // Manually set opcode to 8 (close) — we encoded opcode 2 above.
    frame[0] = static_cast<char>(0x80 | 0x8);
    send_all(fd_, frame);
    close_internal();
}

void WsPeer::close_internal() {
    if (!open_.exchange(false)) return;
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// WsServer
// ---------------------------------------------------------------------------
struct WsServer::Listener final : public MarketDataListener {
    WsServer* server{nullptr};
    void on_event(const Event& e) noexcept override {
        if (server) server->on_publisher_event(e);
    }
    void on_top_of_book(const TopOfBookSnapshot& tob) noexcept override {
        if (server) server->on_publisher_tob(tob.instrument);
    }
};

WsServer::WsServer(MarketDataPublisher& pub, std::uint16_t port)
    : pub_(pub), port_(port), listener_(std::make_unique<Listener>()) {
    listener_->server = this;
}

WsServer::~WsServer() {
    stop();
}

bool WsServer::start() {
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
    running_.store(true, std::memory_order_release);
    pub_.subscribe(listener_.get());
    threads_.emplace_back([this]{ accept_loop(); });
    return true;
}

void WsServer::stop() {
    running_.store(false, std::memory_order_release);
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    // Drop every peer.
    {
        std::lock_guard<std::mutex> lk(peers_mtx_);
        for (auto& [id, peer] : peers_) peer->close(1001);
        peers_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(peer_ids_mtx_);
        peer_ids_.clear();
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    pub_.unsubscribe(listener_.get());
}

void WsServer::accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in caddr{};
        socklen_t   clen  = sizeof(caddr);
        int         cfd   = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cfd < 0) {
            if (!running_.load()) break;
            continue;
        }
        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lk(peer_ids_mtx_);
            id = static_cast<uint64_t>(peer_ids_.size()) + 1;
            peer_ids_.insert(id);
        }
        auto peer = std::make_unique<WsPeer>(cfd, id);
        {
            std::lock_guard<std::mutex> lk(peers_mtx_);
            peers_.emplace(id, std::move(peer));
        }
        threads_.emplace_back([this, id, cfd]{ serve_peer(id, cfd); });
    }
}

void WsServer::serve_peer(uint64_t id, int fd) {
    // 1. Read HTTP upgrade request.
    std::string req;
    char        buf[1024];
    while (req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = read_some(fd, buf, sizeof(buf));
        if (n <= 0) {
            ::close(fd);
            std::lock_guard<std::mutex> lk(peers_mtx_);
            peers_.erase(id);
            return;
        }
        req.append(buf, static_cast<std::size_t>(n));
        if (req.size() > 16 * 1024) {
            ::close(fd);
            std::lock_guard<std::mutex> lk(peers_mtx_);
            peers_.erase(id);
            return;
        }
    }

    std::string method, path, version;
    if (!parse_request_line(req, method, path, version)) {
        ::close(fd);
        std::lock_guard<std::mutex> lk(peers_mtx_);
        peers_.erase(id);
        return;
    }
    if (!ieq(method, "GET")) {
        send_all(fd, "HTTP/1.1 405 Method Not Allowed\r\n\r\n");
        ::close(fd);
        std::lock_guard<std::mutex> lk(peers_mtx_);
        peers_.erase(id);
        return;
    }
    auto key = find_header(req, "Sec-WebSocket-Key");
    if (key.empty()) {
        send_all(fd, "HTTP/1.1 400 Bad Request\r\n\r\n");
        ::close(fd);
        std::lock_guard<std::mutex> lk(peers_mtx_);
        peers_.erase(id);
        return;
    }
    std::string accept = sha1_b64(std::string(key) + std::string(kWsMagic));

    std::string resp;
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: " + accept + "\r\n";
    resp += "\r\n";
    if (!send_all(fd, resp)) {
        ::close(fd);
        std::lock_guard<std::mutex> lk(peers_mtx_);
        peers_.erase(id);
        return;
    }

    // 2. Frame loop. Client→server frames are masked; we strip the mask.
    WsPeer* peer = nullptr;
    {
        std::lock_guard<std::mutex> lk(peers_mtx_);
        auto it = peers_.find(id);
        if (it != peers_.end()) peer = it->second.get();
    }
    if (!peer) return;

    std::string frame;
    while (running_.load(std::memory_order_acquire) && peer->open()) {
        ssize_t n = read_some(fd, buf, sizeof(buf));
        if (n <= 0) break;
        frame.append(buf, static_cast<std::size_t>(n));

        while (frame.size() >= 2) {
            std::uint8_t b0 = static_cast<std::uint8_t>(frame[0]);
            std::uint8_t b1 = static_cast<std::uint8_t>(frame[1]);
            bool fin  = (b0 & 0x80) != 0;
            std::uint8_t opc = b0 & 0x0F;
            bool masked = (b1 & 0x80) != 0;
            std::uint64_t len = b1 & 0x7F;
            std::size_t   need = 2;
            if (len == 126) {
                if (frame.size() < 4) break;
                len = (static_cast<std::uint8_t>(frame[2]) << 8) |
                       static_cast<std::uint8_t>(frame[3]);
                need = 4;
            } else if (len == 127) {
                if (frame.size() < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i)
                    len = (len << 8) | static_cast<std::uint8_t>(frame[static_cast<std::size_t>(2 + i)]);
                need = 10;
            }
            std::size_t mask_len = masked ? 4 : 0;
            if (frame.size() < need + mask_len + len) break;
            std::string payload(frame.data() + need + mask_len,
                                static_cast<std::size_t>(len));
            if (masked) {
                const char* mask = frame.data() + need;
                for (std::size_t i = 0; i < payload.size(); ++i)
                    payload[i] = static_cast<char>(
                        payload[i] ^ mask[i % 4]);
            }
            frame.erase(0, need + mask_len + len);

            if (opc == 0x8) {  // close
                peer->close(1000);
                break;
            } else if (opc == 0x9) {  // ping → pong
                std::string pong;
                ws_encode_server_frame(pong, /*text=*/false, payload);
                pong[0] = static_cast<char>(0x80 | 0xA);
                send_all(fd, pong);
            } else if (opc == 0xA) {  // pong → ignore
                continue;
            } else if (opc == 0x1 || opc == 0x2) {  // text/binary
                if (opc == 0x1 && cbs_.on_text) cbs_.on_text(*peer, payload);
                else if (opc == 0x2 && cbs_.on_binary) cbs_.on_binary(*peer, payload);
            }
            (void)fin;  // we don't support fragmentation
        }
    }

    if (cbs_.on_close) cbs_.on_close(*peer);
    peer->close_internal();
    std::lock_guard<std::mutex> lk(peers_mtx_);
    peers_.erase(id);
    std::lock_guard<std::mutex> lk2(peer_ids_mtx_);
    peer_ids_.erase(id);
}

void WsServer::broadcast_text(const std::string& payload) {
    std::vector<WsPeer*> snapshot;
    {
        std::lock_guard<std::mutex> lk(peers_mtx_);
        snapshot.reserve(peers_.size());
        for (auto& [id, peer] : peers_) snapshot.push_back(peer.get());
    }
    for (WsPeer* p : snapshot) p->send_text(payload);
}

void WsServer::on_publisher_event(const Event& e) {
    if (e.kind == EventKind::Trade) {
        // Serialize trade to JSON and broadcast.
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            R"({"type":"trade","i":%lu,"buy":%lu,"sell":%lu,"px":%lld,"qty":%lld,"seq":%lu})",
            static_cast<unsigned long>(e.trade.instrument_id),
            static_cast<unsigned long>(e.trade.buy_order_id),
            static_cast<unsigned long>(e.trade.sell_order_id),
            static_cast<long long>(e.trade.price.ticks),
            static_cast<long long>(e.trade.quantity.qty),
            static_cast<unsigned long>(e.trade.sequence));
        broadcast_text(buf);
    }
}

void WsServer::on_publisher_tob(InstrumentId id) {
    auto tob = pub_.top_of_book(id);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        R"({"type":"tob","i":%lu,"b":%lld,"bq":%lld,"a":%lld,"aq":%lld,"hb":%d,"ha":%d})",
        static_cast<unsigned long>(tob.instrument),
        tob.has_bid ? static_cast<long long>(tob.bid_price.ticks) : 0,
        tob.has_bid ? static_cast<long long>(tob.bid_qty.qty)    : 0,
        tob.has_ask ? static_cast<long long>(tob.ask_price.ticks) : 0,
        tob.has_ask ? static_cast<long long>(tob.ask_qty.qty)    : 0,
        tob.has_bid ? 1 : 0,
        tob.has_ask ? 1 : 0);
    broadcast_text(buf);
}

}  // namespace tt