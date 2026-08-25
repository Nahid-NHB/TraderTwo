// include/tt/observability/structured_log.hpp
//
// Phase 12: structured logger. Each log line is a JSON object written to
// an arbitrary sink (file, stdout, ring buffer). Used for "human-readable
// but parseable" event records.
//
// The logger is intentionally simple: append-only, mutex-protected,
// non-blocking from the matching hot path (we hand off via an SPSC queue
// if needed — but for Phase 12 we keep it lock-based and accept the cost
// only on the rare admin path).

#pragma once

#include "tt/common/types.hpp"
#include "tt/core/order.hpp"
#include "tt/matching/matching_engine.hpp"   // SubmitResult

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace tt {

enum class LogLevel : uint8_t {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

const char* level_name(LogLevel l) noexcept;

class StructuredLogger {
public:
    // Constructs a logger writing to `path`. Pass "-" for stdout.
    explicit StructuredLogger(std::string path);
    ~StructuredLogger();

    StructuredLogger(const StructuredLogger&)            = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;

    void log(LogLevel level, std::string_view event,
             std::string_view kv_json);

    // Convenience helpers (don't repeat the JSON quoting boilerplate).
    void info(std::string_view event,  std::string_view kv = "{}") { log(LogLevel::Info,  event, kv); }
    void warn(std::string_view event,  std::string_view kv = "{}") { log(LogLevel::Warn,  event, kv); }
    void error(std::string_view event, std::string_view kv = "{}") { log(LogLevel::Error, event, kv); }
    void debug(std::string_view event, std::string_view kv = "{}") { log(LogLevel::Debug, event, kv); }

    void flush();

private:
    void write_line(const std::string& line);

    std::string path_;
    std::FILE*  fp_{nullptr};
    std::mutex  mtx_;
};

// Build a JSON string from an instrument/trade event using snprintf-style
// concatenation. Returned strings are heap-allocated and may be moved.
std::string json_trade(const Trade& t);
std::string json_submit(const SubmitResult& r);

}  // namespace tt