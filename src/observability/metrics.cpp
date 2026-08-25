// src/observability/metrics.cpp

#include "tt/observability/metrics.hpp"

#include <cstdio>
#include <string>

namespace tt {

void MetricsSink::on_trade(const Trade& t) noexcept {
    bundle_.trades_total.fetch_add(1, std::memory_order_relaxed);
    bundle_.trade_qty_total.fetch_add(
        static_cast<std::uint64_t>(t.quantity.qty), std::memory_order_relaxed);
    // Notional = price.ticks * qty.qty (interpreting ticks as price units).
    // Watch for overflow; for tests we keep it raw.
    auto notional = static_cast<std::uint64_t>(t.price.ticks) *
                    static_cast<std::uint64_t>(t.quantity.qty);
    bundle_.trade_notional_total.fetch_add(notional, std::memory_order_relaxed);
    bundle_.last_sequence.store(static_cast<std::uint64_t>(t.sequence),
                                std::memory_order_relaxed);
    if (downstream_) downstream_->on_trade(t);
}

void MetricsSink::on_submit_result(const SubmitResult& r) noexcept {
    switch (r.status) {
        case SubmitStatus::Accepted:
            bundle_.submits_accepted.fetch_add(1, std::memory_order_relaxed);
            break;
        case SubmitStatus::FullyFilled:
            bundle_.submits_filled.fetch_add(1, std::memory_order_relaxed);
            break;
        case SubmitStatus::PartiallyFilled:
            bundle_.submits_partial.fetch_add(1, std::memory_order_relaxed);
            break;
        case SubmitStatus::Cancelled:
            bundle_.submits_cancelled.fetch_add(1, std::memory_order_relaxed);
            break;
        case SubmitStatus::Rejected:
            bundle_.submits_rejected.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    bundle_.last_sequence.store(static_cast<std::uint64_t>(r.sequence),
                                std::memory_order_relaxed);
    if (downstream_) downstream_->on_submit_result(r);
}

void MetricsSink::record_cancel_attempt(bool ok) noexcept {
    bundle_.cancels_attempted.fetch_add(1, std::memory_order_relaxed);
    if (ok) bundle_.cancels_succeeded.fetch_add(1, std::memory_order_relaxed);
}

void MetricsSink::record_modify_attempt(bool ok) noexcept {
    bundle_.modifies_attempted.fetch_add(1, std::memory_order_relaxed);
    if (ok) bundle_.modifies_succeeded.fetch_add(1, std::memory_order_relaxed);
}

std::string render_prometheus(const CounterBundle::Snapshot& s,
                              const std::string& prefix) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "# HELP %s_submits_total Number of submissions by status.\n"
        "# TYPE %s_submits_total counter\n"
        "%s_submits_total{status=\"accepted\"} %lu\n"
        "%s_submits_total{status=\"filled\"}   %lu\n"
        "%s_submits_total{status=\"partial\"}  %lu\n"
        "%s_submits_total{status=\"cancelled\"}%lu\n"
        "%s_submits_total{status=\"rejected\"} %lu\n"
        "# HELP %s_trades_total Number of matched trades.\n"
        "# TYPE %s_trades_total counter\n"
        "%s_trades_total %lu\n"
        "# HELP %s_trade_qty_total Sum of trade quantities.\n"
        "# TYPE %s_trade_qty_total counter\n"
        "%s_trade_qty_total %lu\n"
        "# HELP %s_trade_notional_total Sum of trade notionals (price*qty).\n"
        "# TYPE %s_trade_notional_total counter\n"
        "%s_trade_notional_total %lu\n"
        "# HELP %s_cancels_attempted_total Cancel attempts.\n"
        "# TYPE %s_cancels_attempted_total counter\n"
        "%s_cancels_attempted_total %lu\n"
        "%s_cancels_succeeded_total %lu\n"
        "# HELP %s_modifies_attempted_total Modify attempts.\n"
        "# TYPE %s_modifies_attempted_total counter\n"
        "%s_modifies_attempted_total %lu\n"
        "%s_modifies_succeeded_total %lu\n"
        "# HELP %s_last_sequence Last observed sequence number.\n"
        "# TYPE %s_last_sequence gauge\n"
        "%s_last_sequence %lu\n",
        prefix.c_str(), prefix.c_str(),
        prefix.c_str(), static_cast<unsigned long>(s.submits_accepted),
        prefix.c_str(), static_cast<unsigned long>(s.submits_filled),
        prefix.c_str(), static_cast<unsigned long>(s.submits_partial),
        prefix.c_str(), static_cast<unsigned long>(s.submits_cancelled),
        prefix.c_str(), static_cast<unsigned long>(s.submits_rejected),
        prefix.c_str(), prefix.c_str(),
        prefix.c_str(), static_cast<unsigned long>(s.trades_total),
        prefix.c_str(), prefix.c_str(),
        prefix.c_str(), static_cast<unsigned long>(s.trade_qty_total),
        prefix.c_str(), prefix.c_str(),
        prefix.c_str(), static_cast<unsigned long>(s.trade_notional_total),
        prefix.c_str(), prefix.c_str(),
        prefix.c_str(), static_cast<unsigned long>(s.cancels_attempted),
        prefix.c_str(), static_cast<unsigned long>(s.cancels_succeeded),
        prefix.c_str(), prefix.c_str(),
        prefix.c_str(), static_cast<unsigned long>(s.modifies_attempted),
        prefix.c_str(), static_cast<unsigned long>(s.modifies_succeeded),
        prefix.c_str(), prefix.c_str(),
        prefix.c_str(), static_cast<unsigned long>(s.last_sequence));
    return std::string(buf);
}

std::string render_json(const CounterBundle::Snapshot& s) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        R"({"submits":{"accepted":%lu,"filled":%lu,"partial":%lu,"cancelled":%lu,"rejected":%lu},)"
        R"("trades":{"total":%lu,"qty_total":%lu,"notional_total":%lu},)"
        R"("cancels":{"attempted":%lu,"succeeded":%lu},)"
        R"("modifies":{"attempted":%lu,"succeeded":%lu},)"
        R"("last_sequence":%lu})",
        static_cast<unsigned long>(s.submits_accepted),
        static_cast<unsigned long>(s.submits_filled),
        static_cast<unsigned long>(s.submits_partial),
        static_cast<unsigned long>(s.submits_cancelled),
        static_cast<unsigned long>(s.submits_rejected),
        static_cast<unsigned long>(s.trades_total),
        static_cast<unsigned long>(s.trade_qty_total),
        static_cast<unsigned long>(s.trade_notional_total),
        static_cast<unsigned long>(s.cancels_attempted),
        static_cast<unsigned long>(s.cancels_succeeded),
        static_cast<unsigned long>(s.modifies_attempted),
        static_cast<unsigned long>(s.modifies_succeeded),
        static_cast<unsigned long>(s.last_sequence));
    return std::string(buf);
}

}  // namespace tt