// include/tt/persistence/event_log.hpp
//
// Append-only event log + replay. Each event is written as a typed, fixed-
// sized binary record tagged with the engine's monotonic sequence number.
//
// The log is intended for crash recovery and (later) for offline
// analytics. It is not a WAL in the database sense — there are no
// per-record CRCs, no transactional grouping, no idempotency keys. The
// engine is single-threaded per instrument, so on crash we replay the log
// in order to reconstruct state.

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/market_data/events.hpp"
#include "tt/matching/matching_engine.hpp"   // SubmitResult, MatchingEngine

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace tt {

// One persisted event record. Keep it POD-ish for trivial disk I/O.
struct PersistedEvent {
    Sequence sequence{0};
    Timestamp timestamp{0};
    EventKind kind{EventKind::OrderAccepted};

    // Discriminated payload. The fields below are interpreted based on `kind`.
    OrderPayload order{};
    Trade        trade{};
    std::string  reject_reason;  // only when kind == OrderRejected
};

// Lightweight binary log. Format:
//   u64  magic    (0x54545631 "TTV1")
//   u64  version  (currently 1)
//   then a sequence of records:
//     u8 kind
//     u64 sequence
//     u64 timestamp
//     bytes payload
//
// Replay reads the whole file into memory and returns the records in
// insertion order. This is sufficient for tests and the demo client; a real
// deployment would use mmap + chunked read.

class EventLog {
public:
    // If append is false, the file is truncated and the header written.
    // If append is true, the existing header is validated and the file
    // pointer is positioned at end.
    explicit EventLog(std::string path, bool append = false);
    ~EventLog();

    EventLog(const EventLog&)            = delete;
    EventLog& operator=(const EventLog&) = delete;

    // Append a record. Thread-unsafe: call from the matching thread.
    void append(const PersistedEvent& e);

    // Close the file so a fresh `read_all` can be opened. Optional.
    void close();

    // Re-read the file from disk and return its records. Used by the
    // recovery tool. The file is closed for writes during replay.
    std::vector<PersistedEvent> read_all();

    // Total bytes written so far. For instrumentation.
    std::uint64_t bytes_written() const noexcept { return bytes_written_; }

private:
    std::string path_;
    std::FILE*  fp_{nullptr};
    std::uint64_t bytes_written_{0};

    static constexpr std::uint64_t kMagic   = 0x54545631; // "TTV1"
    static constexpr std::uint64_t kVersion = 1;
};

// A persisting sink that writes each event to an EventLog.
class PersistingSink final : public TradeSink {
public:
    explicit PersistingSink(EventLog& log) : log_(log) {}
    void on_trade(const Trade& t) noexcept override;
    void on_submit_result(const SubmitResult& r) noexcept override;

private:
    EventLog& log_;
};

// Replay records back into a fresh MatchingEngine. The engine is reset
// first; we replay each record in order. Cancels/rest/fills are turned
// back into engine API calls.
class Replayer {
public:
    explicit Replayer(MatchingEngine& eng) : engine_(eng) {}

    // Apply a sequence of records. Stops on the first unsupported event
    // kind and returns the index of the failed record (or count on full
    // success).
    std::size_t replay(const std::vector<PersistedEvent>& records);

private:
    MatchingEngine& engine_;
};

}  // namespace tt