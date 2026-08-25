// src/observability/structured_log.cpp

#include "tt/observability/structured_log.hpp"

#include "tt/matching/matching_engine.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace tt {

const char* level_name(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

namespace {

// Minimal JSON string escaper: quote, backslash, and control chars.
std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string timestamp_iso8601() {
    using namespace std::chrono;
    auto now    = system_clock::now();
    auto secs   = time_point_cast<seconds>(now);
    auto micros = duration_cast<microseconds>(now - secs).count();
    auto tt     = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long>(micros));
    return std::string(buf);
}

}  // namespace

StructuredLogger::StructuredLogger(std::string path) : path_(std::move(path)) {
    if (path_ == "-") {
        fp_ = stdout;
    } else {
        fp_ = std::fopen(path_.c_str(), "a");
        if (!fp_) fp_ = stdout;  // graceful degradation
    }
}

StructuredLogger::~StructuredLogger() {
    if (fp_ && fp_ != stdout) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void StructuredLogger::write_line(const std::string& line) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!fp_) return;
    std::fwrite(line.data(), 1, line.size(), fp_);
    std::fputc('\n', fp_);
}

void StructuredLogger::log(LogLevel level, std::string_view event,
                           std::string_view kv_json) {
    char header[160];
    std::snprintf(header, sizeof(header),
                  R"({"ts":"%s","level":"%s","event":"%s",)",
                  timestamp_iso8601().c_str(),
                  level_name(level),
                  escape(event).c_str());
    std::string line;
    line.reserve(160 + kv_json.size());
    line += header;
    // Trim a trailing '}' from kv if present, we'll close the object ourselves.
    std::string_view body = kv_json;
    if (!body.empty() && body.back() == '}') body.remove_suffix(1);
    line.append(body.data(), body.size());
    line += "}\n";
    write_line(line);
}

void StructuredLogger::flush() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fp_) std::fflush(fp_);
}

std::string json_trade(const Trade& t) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        R"({"buy_order_id":%lu,"sell_order_id":%lu,"price_ticks":%lld,"qty":%lld,"sequence":%lu,"instrument":%lu})",
        static_cast<unsigned long>(t.buy_order_id),
        static_cast<unsigned long>(t.sell_order_id),
        static_cast<long long>(t.price.ticks),
        static_cast<long long>(t.quantity.qty),
        static_cast<unsigned long>(t.sequence),
        static_cast<unsigned long>(t.instrument_id));
    return std::string(buf);
}

std::string json_submit(const SubmitResult& r) {
    char buf[256];
    const char* status = "accepted";
    switch (r.status) {
        case SubmitStatus::Accepted:        status = "accepted";   break;
        case SubmitStatus::FullyFilled:     status = "filled";     break;
        case SubmitStatus::PartiallyFilled: status = "partial";    break;
        case SubmitStatus::Cancelled:       status = "cancelled";  break;
        case SubmitStatus::Rejected:        status = "rejected";   break;
    }
    std::snprintf(buf, sizeof(buf),
        R"({"order_id":%lu,"sequence":%lu,"status":"%s","instrument":%lu,"filled":%lld,"resting":%lld,"price_ticks":%lld,"reject_reason":"%s"})",
        static_cast<unsigned long>(r.order_id),
        static_cast<unsigned long>(r.sequence),
        status,
        static_cast<unsigned long>(r.instrument_id),
        static_cast<long long>(r.filled_quantity.qty),
        static_cast<long long>(r.resting_quantity.qty),
        static_cast<long long>(r.price.ticks),
        escape(r.reject_reason).c_str());
    return std::string(buf);
}

}  // namespace tt