// src/persistence/event_log.cpp

#include "tt/persistence/event_log.hpp"

#include "tt/matching/matching_engine.hpp"

#include <cassert>
#include <cstring>
#include <memory>
#include <utility>

namespace tt {

namespace {

void write_u64(std::FILE* fp, std::uint64_t v) {
    std::fwrite(&v, sizeof(v), 1, fp);
}

std::uint64_t read_u64(std::FILE* fp) {
    std::uint64_t v = 0;
    std::fread(&v, sizeof(v), 1, fp);
    return v;
}

void write_u8(std::FILE* fp, std::uint8_t v) {
    std::fwrite(&v, sizeof(v), 1, fp);
}

std::uint8_t read_u8(std::FILE* fp) {
    std::uint8_t v = 0;
    std::fread(&v, sizeof(v), 1, fp);
    return v;
}

void write_str(std::FILE* fp, const std::string& s) {
    std::uint32_t len = static_cast<std::uint32_t>(s.size());
    std::fwrite(&len, sizeof(len), 1, fp);
    if (len) std::fwrite(s.data(), 1, len, fp);
}

std::string read_str(std::FILE* fp) {
    std::uint32_t len = 0;
    std::fread(&len, sizeof(len), 1, fp);
    std::string s(len, '\0');
    if (len) std::fread(s.data(), 1, len, fp);
    return s;
}

void write_payload(std::FILE* fp, const PersistedEvent& e) {
    write_u8(fp, static_cast<std::uint8_t>(e.kind));
    write_u64(fp, e.sequence);
    write_u64(fp, e.timestamp);

    switch (e.kind) {
        case EventKind::Trade:
            write_u64(fp, e.trade.id);
            write_u64(fp, e.trade.instrument_id);
            write_u64(fp, e.trade.buy_order_id);
            write_u64(fp, e.trade.sell_order_id);
            write_u64(fp, e.trade.buy_trader_id);
            write_u64(fp, e.trade.sell_trader_id);
            {
                std::int64_t v = e.trade.price.ticks;
                std::fwrite(&v, sizeof(v), 1, fp);
            }
            {
                std::int64_t v = e.trade.quantity.qty;
                std::fwrite(&v, sizeof(v), 1, fp);
            }
            break;
        default:
            // Order events share the OrderPayload schema.
            write_u64(fp, e.order.order_id);
            write_u64(fp, e.order.instrument);
            write_u8(fp,  static_cast<std::uint8_t>(e.order.side));
            write_u8(fp,  static_cast<std::uint8_t>(e.order.type));
            {
                std::int64_t v = e.order.price.ticks;
                std::fwrite(&v, sizeof(v), 1, fp);
            }
            {
                std::int64_t v = e.order.qty.qty;
                std::fwrite(&v, sizeof(v), 1, fp);
            }
            write_str(fp, e.reject_reason);
            break;
    }
}

PersistedEvent read_payload(std::FILE* fp) {
    PersistedEvent e{};
    e.kind      = static_cast<EventKind>(read_u8(fp));
    e.sequence  = read_u64(fp);
    e.timestamp = read_u64(fp);

    switch (e.kind) {
        case EventKind::Trade: {
            e.trade.id            = read_u64(fp);
            e.trade.instrument_id = read_u64(fp);
            e.trade.buy_order_id  = read_u64(fp);
            e.trade.sell_order_id = read_u64(fp);
            e.trade.buy_trader_id = read_u64(fp);
            e.trade.sell_trader_id= read_u64(fp);
            std::int64_t price_ticks = 0;
            std::fread(&price_ticks, sizeof(price_ticks), 1, fp);
            e.trade.price.ticks = price_ticks;
            std::int64_t qty = 0;
            std::fread(&qty, sizeof(qty), 1, fp);
            e.trade.quantity.qty = qty;
            break;
        }
        default: {
            e.order.order_id    = read_u64(fp);
            e.order.instrument  = read_u64(fp);
            e.order.side        = static_cast<Side>(read_u8(fp));
            e.order.type        = static_cast<OrderType>(read_u8(fp));
            std::int64_t price_ticks = 0;
            std::fread(&price_ticks, sizeof(price_ticks), 1, fp);
            e.order.price.ticks = price_ticks;
            std::int64_t qty = 0;
            std::fread(&qty, sizeof(qty), 1, fp);
            e.order.qty.qty     = qty;
            e.reject_reason     = read_str(fp);
            break;
        }
    }
    return e;
}

}  // namespace

EventLog::EventLog(std::string path, bool append)
    : path_(std::move(path)), fp_(nullptr) {
    const char* mode = append ? "ab" : "wb";
    fp_ = std::fopen(path_.c_str(), mode);
    if (!fp_) return;
    if (!append) {
        write_u64(fp_, kMagic);
        write_u64(fp_, kVersion);
        bytes_written_ += 16;
    } else {
        // Validate header on append. In "ab" mode the file position is at
        // the end of file, so rewind first.
        std::rewind(fp_);
        std::uint64_t magic = read_u64(fp_);
        std::uint64_t version = read_u64(fp_);
        if (magic != kMagic || version != kVersion) {
            std::fclose(fp_);
            fp_ = nullptr;
            return;
        }
        // "ab" mode always writes at end of file regardless of position,
        // so we don't need to seek back. Just record current bytes.
        std::fseek(fp_, 0, SEEK_END);
        bytes_written_ = static_cast<std::uint64_t>(std::ftell(fp_));
    }
}

EventLog::~EventLog() {
    if (fp_) std::fclose(fp_);
}

void EventLog::close() {
    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void EventLog::append(const PersistedEvent& e) {
    if (!fp_) return;
    std::uint64_t before = bytes_written_;
    write_payload(fp_, e);
    std::fflush(fp_);
    bytes_written_ = before + /* recompute below */ 0;
    // We don't track byte-by-byte accounting; cheap approximation:
    bytes_written_ = static_cast<std::uint64_t>(std::ftell(fp_));
}

std::vector<PersistedEvent> EventLog::read_all() {
    std::vector<PersistedEvent> out;
    // Flush and reopen in read-only mode to avoid truncating the file.
    if (fp_) {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }
    std::FILE* fp = std::fopen(path_.c_str(), "rb");
    if (!fp) return out;

    std::uint64_t magic = read_u64(fp);
    std::uint64_t version = read_u64(fp);
    if (magic != kMagic || version != kVersion) {
        std::fclose(fp);
        return out;
    }

    while (!std::feof(fp)) {
        long pos = std::ftell(fp);
        PersistedEvent e = read_payload(fp);
        if (std::ftell(fp) == pos) break;  // no progress, EOF
        if (std::feof(fp)) {
            // Include if we read something but EOF after.
            out.push_back(std::move(e));
            break;
        }
        out.push_back(std::move(e));
    }
    std::fclose(fp);
    return out;
}

// ---------------------------------------------------------------------------
// PersistingSink
// ---------------------------------------------------------------------------
void PersistingSink::on_trade(const Trade& t) noexcept {
    PersistedEvent e{};
    e.sequence  = t.sequence;
    e.timestamp = t.timestamp;
    e.kind      = EventKind::Trade;
    e.trade     = t;
    log_.append(e);
}

void PersistingSink::on_submit_result(const SubmitResult& r) noexcept {
    PersistedEvent e{};
    e.sequence  = r.sequence;
    e.timestamp = 0;
    e.order.order_id   = r.order_id;
    e.order.instrument = r.instrument_id;
    e.order.side       = r.side;
    e.order.type       = r.type;
    e.order.price      = r.price;
    // Persist the original order quantity (filled + resting) so replay can
    // reconstruct the order.
    e.order.qty        = Quantity{r.filled_quantity.qty + r.resting_quantity.qty};
    e.reject_reason    = r.reject_reason;
    switch (r.status) {
        case SubmitStatus::Accepted:
            e.kind = EventKind::OrderRested; break;
        case SubmitStatus::FullyFilled:
            e.kind = EventKind::OrderFilled; break;
        case SubmitStatus::PartiallyFilled:
            e.kind = EventKind::OrderPartiallyFilled; break;
        case SubmitStatus::Cancelled:
            e.kind = EventKind::OrderCancelled; break;
        case SubmitStatus::Rejected:
            e.kind = EventKind::OrderRejected; break;
    }
    log_.append(e);
}

// ---------------------------------------------------------------------------
// Replayer
// ---------------------------------------------------------------------------
std::size_t Replayer::replay(const std::vector<PersistedEvent>& records) {
    // A replayer that rebuilds the book from a stream of records is
    // intrinsically complex: cancel/modify events need to be re-applied,
    // fills need a non-trader "passive" order to fire against. For the
    // purposes of this project, replay is implemented at the engine API
    // level for the simplest subset: a stream of new orders. Anything
    // else is returned as "unsupported".
    //
    // A full event-sourced reconstruction is left as a future phase.

    std::size_t applied = 0;
    for (const auto& e : records) {
        if (e.kind == EventKind::Trade) {
            ++applied;
            continue;
        }
        if (e.kind == EventKind::OrderRejected) {
            ++applied;
            continue;
        }
        if (e.kind == EventKind::OrderRested ||
            e.kind == EventKind::OrderFilled ||
            e.kind == EventKind::OrderPartiallyFilled) {
            // Convert to a fresh submit.
            auto o = std::make_unique<Order>();
            o->id            = e.order.order_id;
            o->trader_id     = 0; // unknown on replay
            o->instrument_id = e.order.instrument;
            o->side          = e.order.side;
            o->type          = (e.order.qty.qty > 0)
                                  ? OrderType::Limit
                                  : OrderType::Market;
            o->tif           = TimeInForce::GTC;
            o->price         = e.order.price;
            o->quantity      = e.order.qty;
            o->remaining     = e.order.qty;
            o->status        = OrderStatus::New;
            CollectingSink sink;
            engine_.submit(std::move(o), sink);
            ++applied;
            continue;
        }
        // Unknown / not yet supported.
        break;
    }
    return applied;
}

}  // namespace tt