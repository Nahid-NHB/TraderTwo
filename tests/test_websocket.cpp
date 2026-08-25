// tests/test_websocket.cpp
//
// End-to-end tests for the WebSocket server. We connect to the WS server
// using a tiny inline client (raw sockets + handshake + frame encoding),
// drive trades through the matching engine, and verify that the server
// broadcasts trade + top-of-book messages over the WebSocket.

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/market_data/publisher.hpp"
#include "tt/matching/matching_engine.hpp"
#include "tt/networking/ws_server.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace tt;

namespace {

// ---------- Tiny inline WS client -----------------------------------------
class WsTestClient {
public:
    WsTestClient() : fd_(-1) {}
    ~WsTestClient() { close(); }

    bool connect(const char* host, std::uint16_t port, const std::string& path = "/") {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port   = htons(port);
        inet_pton(AF_INET, host, &a.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + std::string(host) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        if (!send_all(req)) { ::close(fd_); fd_ = -1; return false; }

        // Read response until "\r\n\r\n"
        std::string resp;
        char buf[1024];
        while (resp.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) { ::close(fd_); fd_ = -1; return false; }
            resp.append(buf, static_cast<std::size_t>(n));
        }
        return resp.find("101") != std::string::npos;
    }

    void close() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    // Send a text frame (client must mask).
    bool send_text(const std::string& payload) {
        std::string out;
        out.push_back(static_cast<char>(0x81));  // FIN + text
        std::uint64_t len = payload.size();
        if (len <= 125) {
            out.push_back(static_cast<char>(0x80 | len));  // mask bit set
        } else if (len <= 0xFFFF) {
            out.push_back(static_cast<char>(0x80 | 126));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(len & 0xFF));
        } else {
            return false;
        }
        // Mask key (4 bytes) — use a fixed mask for testing.
        const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
        out.append(reinterpret_cast<const char*>(mask), 4);
        std::string masked = payload;
        for (std::size_t i = 0; i < masked.size(); ++i)
            masked[i] = static_cast<char>(masked[i] ^ mask[i % 4]);
        out.append(masked);
        return send_all(out);
    }

    // Receive one text frame (non-blocking-ish with a timeout). Uses an
    // internal buffer so bytes from a previous recv that belong to the next
    // frame aren't dropped.
    bool recv_text(std::string& payload, int timeout_ms = 1000) {
        timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char b[2048];
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (true) {
            // First, see if we already have a complete frame buffered.
            if (try_extract_frame(payload)) return true;

            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            // Cap the recv timeout to whatever's left of our overall budget.
            timeval rt{ remaining / 1000, (remaining % 1000) * 1000 };
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));

            ssize_t n = ::recv(fd_, b, sizeof(b), 0);
            if (n > 0) {
                rx_buf_.append(b, static_cast<std::size_t>(n));
            } else if (n == 0) {
                return false;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
            }
        }
    }

private:
    // Returns true if a full text frame is present in rx_buf_; on success
    // payload is set and the consumed bytes are removed from rx_buf_.
    bool try_extract_frame(std::string& payload) {
        if (rx_buf_.size() < 2) return false;
        std::uint8_t b0 = static_cast<std::uint8_t>(rx_buf_[0]);
        std::uint8_t b1 = static_cast<std::uint8_t>(rx_buf_[1]);
        bool masked = (b1 & 0x80) != 0;
        std::uint64_t len = b1 & 0x7F;
        std::size_t need = 2;
        if (len == 126) {
            if (rx_buf_.size() < 4) return false;
            len = (static_cast<std::uint8_t>(rx_buf_[2]) << 8) |
                   static_cast<std::uint8_t>(rx_buf_[3]);
            need = 4;
        } else if (len == 127) {
            if (rx_buf_.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) |
                      static_cast<std::uint8_t>(rx_buf_[static_cast<std::size_t>(2 + i)]);
            need = 10;
        }
        std::size_t mask_len = masked ? 4 : 0;
        if (rx_buf_.size() < need + mask_len + len) return false;

        payload.assign(rx_buf_.data() + need + mask_len,
                       static_cast<std::size_t>(len));
        if (masked) {
            const char* m = rx_buf_.data() + need;
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(payload[i] ^ m[i % 4]);
        }
        rx_buf_.erase(0, need + mask_len + len);
        (void)b0;
        return true;
    }

    bool send_all(const std::string& s) {
        std::size_t sent = 0;
        while (sent < s.size()) {
            ssize_t k = ::send(fd_, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
            if (k <= 0) return false;
            sent += static_cast<std::size_t>(k);
        }
        return true;
    }

    int       fd_;
    std::string rx_buf_;
};

// ---------- Test fixture --------------------------------------------------
struct WsFixture {
    MatchingEngine engine;
    MarketDataPublisher pub;
    // Pipe trades through the publisher so the WS listener sees them.
    struct Pipe final : public TradeSink {
        TradeSink* a;
        TradeSink* b;
        Pipe(TradeSink* x, TradeSink* y) : a(x), b(y) {}
        void on_trade(const Trade& t) noexcept override {
            a->on_trade(t);
            b->on_trade(t);
        }
        void on_submit_result(const SubmitResult& r) noexcept override {
            a->on_submit_result(r);
            b->on_submit_result(r);
        }
    };
    CollectingSink collecting;
    Pipe sink;
    WsServer ws;

    WsFixture(std::uint16_t port)
        : pub(engine), sink(&collecting, &pub), ws(pub, port) {
        engine.register_instrument(InstrumentId{1});
        engine.register_instrument(InstrumentId{2});
    }

    bool start() { return ws.start(); }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
TEST(WsServer, HandshakeAndOneClient) {
    WsFixture f(/*port=*/19700);
    ASSERT_TRUE(f.start());

    WsTestClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", 19700));
    // After connect the server should at least have registered the peer.
    EXPECT_TRUE(f.ws.running());
    client.close();
    f.ws.stop();
}

TEST(WsServer, FrameEncodingHelper) {
    std::string out;
    auto n = ws_encode_server_frame(out, /*text=*/true, "hello");
    EXPECT_GT(n, 0u);
    EXPECT_EQ(out[0], static_cast<char>(0x81));        // FIN + text
    EXPECT_EQ(out[1] & 0x80, 0);                        // server frames un-masked
    EXPECT_EQ(out[1] & 0x7F, 5u);                      // length 5
    EXPECT_EQ(out.substr(2), "hello");
}

TEST(WsServer, BroadcastsTrade) {
    WsFixture f(/*port=*/19701);
    ASSERT_TRUE(f.start());

    WsTestClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", 19701));

    // Drive a crossing trade so the publisher emits an event.
    f.engine.submit_limit(InstrumentId{1}, TraderId{1}, Side::Sell,
                          Price{100}, Quantity{5}, TimeInForce::GTC, f.sink);
    f.engine.submit_limit(InstrumentId{1}, TraderId{2}, Side::Buy,
                          Price{100}, Quantity{3}, TimeInForce::GTC, f.sink);

    // The listener fires synchronously inside submit(); we may already have
    // a frame buffered in the socket. Read messages for up to 1s.
    std::string frame;
    bool got_trade = false;
    for (int i = 0; i < 20; ++i) {
        if (client.recv_text(frame, 200)) {
            if (frame.find("\"type\":\"trade\"") != std::string::npos) {
                got_trade = true;
                break;
            }
        }
    }
    EXPECT_TRUE(got_trade) << "expected a trade broadcast";

    client.close();
    f.ws.stop();
}

TEST(WsServer, BroadcastsTopOfBook) {
    WsFixture f(/*port=*/19702);
    ASSERT_TRUE(f.start());

    WsTestClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", 19702));

    f.engine.submit_limit(InstrumentId{2}, TraderId{1}, Side::Sell,
                          Price{200}, Quantity{7}, TimeInForce::GTC, f.sink);

    std::string frame;
    bool got_tob = false;
    for (int i = 0; i < 20; ++i) {
        if (client.recv_text(frame, 200)) {
            if (frame.find("\"type\":\"tob\"") != std::string::npos &&
                frame.find("\"i\":2") != std::string::npos) {
                got_tob = true;
                break;
            }
        }
    }
    EXPECT_TRUE(got_tob) << "expected a top-of-book broadcast for instrument 2";

    client.close();
    f.ws.stop();
}

TEST(WsServer, ClientTextFrameArrivesToCallbacks) {
    WsFixture f(/*port=*/19703);
    ASSERT_TRUE(f.start());

    std::mutex mtx;
    std::vector<std::string> received;
    f.ws.set_callbacks({
        .on_text = [&](WsPeer&, const std::string& t) {
            std::lock_guard<std::mutex> lk(mtx);
            received.push_back(t);
        },
        .on_binary = nullptr,
        .on_close  = nullptr,
    });

    WsTestClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", 19703));
    ASSERT_TRUE(client.send_text("hello from client"));

    // Give the server a moment to dispatch the callback.
    for (int i = 0; i < 30 && received.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
        std::lock_guard<std::mutex> lk(mtx);
        ASSERT_FALSE(received.empty());
        EXPECT_EQ(received.front(), "hello from client");
    }

    client.close();
    f.ws.stop();
}

TEST(WsServer, MultipleClientsAllReceive) {
    WsFixture f(/*port=*/19704);
    ASSERT_TRUE(f.start());

    WsTestClient a, b;
    ASSERT_TRUE(a.connect("127.0.0.1", 19704));
    ASSERT_TRUE(b.connect("127.0.0.1", 19704));

    f.engine.submit_limit(InstrumentId{1}, TraderId{1}, Side::Sell,
                          Price{100}, Quantity{5}, TimeInForce::GTC, f.sink);
    f.engine.submit_limit(InstrumentId{1}, TraderId{2}, Side::Buy,
                          Price{100}, Quantity{2}, TimeInForce::GTC, f.sink);

    int got_a = 0, got_b = 0;
    std::string f1, f2;
    for (int i = 0; i < 20 && (got_a == 0 || got_b == 0); ++i) {
        if (got_a == 0 && a.recv_text(f1, 200)) ++got_a;
        if (got_b == 0 && b.recv_text(f2, 200)) ++got_b;
    }
    EXPECT_GT(got_a, 0);
    EXPECT_GT(got_b, 0);

    a.close();
    b.close();
    f.ws.stop();
}

}  // namespace