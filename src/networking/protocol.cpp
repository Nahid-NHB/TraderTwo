// src/networking/protocol.cpp

#include "tt/networking/protocol.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace tt {

namespace {

bool ieq(std::string_view a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

}  // namespace

ParseError parse_request(std::string_view line, ParsedRequest& out) {
    ParseError err;
    if (line.empty()) { err.ok = false; err.message = "empty line"; return err; }
    // Tokenise by whitespace.
    std::vector<std::string_view> tok;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        std::size_t begin = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        if (begin < pos) tok.emplace_back(line.data() + begin, pos - begin);
    }
    if (tok.empty()) { err.ok = false; err.message = "no tokens"; return err; }

    auto need = [&](std::size_t n) -> bool {
        if (tok.size() < n) {
            err.ok = false;
            err.message = "missing arguments";
            return false;
        }
        return true;
    };
    auto parse_u64 = [&](std::string_view s, std::uint64_t& v) -> bool {
        if (s.empty()) return false;
        std::uint64_t x = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            x = x * 10 + static_cast<std::uint64_t>(c - '0');
        }
        v = x;
        return true;
    };
    auto parse_i64 = [&](std::string_view s, std::int64_t& v) -> bool {
        if (s.empty()) return false;
        bool neg = (s[0] == '-');
        std::size_t start = neg ? 1 : 0;
        if (start == s.size()) return false;
        std::int64_t x = 0;
        for (std::size_t i = start; i < s.size(); ++i) {
            char c = s[i];
            if (c < '0' || c > '9') return false;
            x = x * 10 + (c - '0');
        }
        v = neg ? -x : x;
        return true;
    };

    std::string_view cmd = tok[0];
    if (ieq(cmd, "PING")) {
        out.command = InboundCommand::Ping;
        return err;
    }
    if (ieq(cmd, "QUOTE")) {
        if (!need(2)) return err;
        if (!parse_u64(tok[1], reinterpret_cast<std::uint64_t&>(out.instrument))) {
            err.ok = false; err.message = "bad instrument"; return err;
        }
        out.command = InboundCommand::Quote;
        return err;
    }
    if (ieq(cmd, "NEW")) {
        if (!need(6)) return err;
        std::uint64_t instr = 0, trader = 0, side_u = 0, price = 0, qty = 0;
        if (!parse_u64(tok[1], instr))  { err.ok = false; err.message = "bad instrument"; return err; }
        if (!parse_u64(tok[2], trader)) { err.ok = false; err.message = "bad trader";    return err; }
        if (!parse_u64(tok[3], side_u)) { err.ok = false; err.message = "bad side";      return err; }
        if (!parse_u64(tok[4], price))  { err.ok = false; err.message = "bad price";     return err; }
        if (!parse_u64(tok[5], qty))    { err.ok = false; err.message = "bad qty";       return err; }
        out.instrument = static_cast<InstrumentId>(instr);
        out.trader_id  = static_cast<TraderId>(trader);
        out.side       = (side_u == 0) ? Side::Buy : Side::Sell;
        out.type       = OrderType::Limit;
        out.price      = Price{static_cast<int64_t>(price)};
        out.qty        = Quantity{static_cast<int64_t>(qty)};
        out.tif        = TimeInForce::GTC;
        if (tok.size() >= 7) {
            std::string_view tif = tok[6];
            if      (ieq(tif, "IOC")) out.tif = TimeInForce::IOC;
            else if (ieq(tif, "FOK")) out.tif = TimeInForce::FOK;
            else if (ieq(tif, "GTC")) out.tif = TimeInForce::GTC;
            else { err.ok = false; err.message = "bad tif"; return err; }
        }
        out.command = InboundCommand::New;
        return err;
    }
    if (ieq(cmd, "CANCEL")) {
        if (!need(3)) return err;
        std::uint64_t instr = 0, oid = 0;
        if (!parse_u64(tok[1], instr)) { err.ok = false; err.message = "bad instrument"; return err; }
        if (!parse_u64(tok[2], oid))   { err.ok = false; err.message = "bad order_id";   return err; }
        out.instrument = static_cast<InstrumentId>(instr);
        out.order_id   = static_cast<OrderId>(oid);
        out.command    = InboundCommand::Cancel;
        return err;
    }
    if (ieq(cmd, "MODIFY")) {
        if (!need(5)) return err;
        std::uint64_t instr = 0, oid = 0, qty_u = 0;
        std::int64_t  price = 0;
        if (!parse_u64(tok[1], instr)) { err.ok = false; err.message = "bad instrument"; return err; }
        if (!parse_u64(tok[2], oid))   { err.ok = false; err.message = "bad order_id";   return err; }
        if (!parse_u64(tok[3], qty_u)) { err.ok = false; err.message = "bad qty";        return err; }
        if (!parse_i64(tok[4], price)) { err.ok = false; err.message = "bad price";      return err; }
        out.instrument = static_cast<InstrumentId>(instr);
        out.order_id   = static_cast<OrderId>(oid);
        out.qty        = Quantity{static_cast<int64_t>(qty_u)};
        out.price      = Price{price};
        out.command    = InboundCommand::Modify;
        return err;
    }
    err.ok = false;
    err.message = "unknown command";
    return err;
}

std::string format_submit_reply(OrderId id, Sequence seq, const std::string& status) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK %s %lu %lu\n",
                  status.c_str(), static_cast<unsigned long>(id),
                  static_cast<unsigned long>(seq));
    return std::string(buf);
}

std::string format_trade(const Trade& t) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "TRADE %lu %lu %lld %lld\n",
                  static_cast<unsigned long>(t.buy_order_id),
                  static_cast<unsigned long>(t.sell_order_id),
                  static_cast<long long>(t.price.ticks),
                  static_cast<long long>(t.quantity.qty));
    return std::string(buf);
}

std::string format_quote(InstrumentId id, Price bid, Quantity bid_qty,
                         Price ask, Quantity ask_qty) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "QUOTE %lu %lld %lld %lld %lld\n",
                  static_cast<unsigned long>(id),
                  static_cast<long long>(bid.ticks),
                  static_cast<long long>(bid_qty.qty),
                  static_cast<long long>(ask.ticks),
                  static_cast<long long>(ask_qty.qty));
    return std::string(buf);
}

std::string format_error(const std::string& msg) {
    return "ERR " + msg + "\n";
}

}  // namespace tt