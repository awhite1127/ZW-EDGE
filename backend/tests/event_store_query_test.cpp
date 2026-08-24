#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

#include "common/time_utils.h"
#include "datastore/event_store.h"

namespace {

using edge_controller::EventHistoryQuery;
using edge_controller::EventHistoryResult;
using edge_controller::EventStore;
using edge_controller::ServiceEvent;
using edge_controller::StatusCode;
using edge_controller::TimestampMs;
using edge_controller::is_ok;

ServiceEvent make_event(
    TimestampMs timestamp_ms,
    std::string level,
    std::string source,
    std::string target_id,
    std::string summary,
    std::string detail = {},
    std::string suggestion = {})
{
    ServiceEvent event;
    event.timestamp_ms = timestamp_ms;
    event.level = std::move(level);
    event.source = std::move(source);
    event.target_id = std::move(target_id);
    event.summary = std::move(summary);
    event.detail = std::move(detail);
    event.diagnosis.target_id = event.target_id;
    event.diagnosis.message = event.summary;
    event.diagnosis.suggestion = std::move(suggestion);
    return event;
}

bool expect(bool condition, const std::string& message)
{
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::uint64_t source_count(const EventHistoryResult& result, const std::string& source)
{
    for (const auto& stat : result.source_stats) {
        if (stat.source == source) return stat.count;
    }
    return 0;
}

}  // namespace

int main()
{
    EventStore store;
    std::string error;
    if (!is_ok(store.initialize(":memory:", &error))) {
        std::cerr << "initialize failed: " << error << '\n';
        return 1;
    }

    const auto now = edge_controller::time_utils::system_now_ms();
    const auto minute = static_cast<TimestampMs>(60'000);
    const auto day = static_cast<TimestampMs>(24ULL * 60ULL * 60ULL * 1000ULL);
    const ServiceEvent events[] = {
        make_event(now - minute, "error", "device", "dev-a", "load reached 100%"),
        make_event(now - 2 * minute, "warning", "polling", "dev-b", "poll warning", {}, "check ground fault"),
        make_event(now - 3 * minute, "warn", "polling", "dev-c", "poll retry", "ground connection unstable"),
        make_event(now - 4 * minute, "info", "system", "system", "service started"),
        make_event(now - 8 * day, "custom", "config_apply", "config", "legacy config notice"),
    };
    for (const auto& event : events) {
        if (!is_ok(store.append(event, &error))) {
            std::cerr << "append failed: " << error << '\n';
            return 1;
        }
    }

    EventHistoryResult result;
    EventHistoryQuery page_query;
    page_query.page = 2;
    page_query.page_size = 2;
    if (!is_ok(store.query_history(page_query, &result, &error))) {
        std::cerr << "page query failed: " << error << '\n';
        return 1;
    }
    if (!expect(result.total == 5, "page total must cover the complete retained event set") ||
        !expect(result.rows.size() == 2, "second page must contain two rows") ||
        !expect(result.rows[0].target_id == "dev-c" && result.rows[1].target_id == "system", "rows must be timestamp-descending") ||
        !expect(result.level_stats.error == 1 && result.level_stats.warning == 2 && result.level_stats.info == 2, "level stats must normalize warn and unknown levels") ||
        !expect(source_count(result, "polling") == 2 && source_count(result, "device") == 1, "source stats must cover all retained events")) {
        return 1;
    }

    EventHistoryQuery filtered_query;
    filtered_query.level = "warning";
    filtered_query.source = "POLLING";
    filtered_query.time_range = "24h";
    filtered_query.search = "ground";
    filtered_query.page_size = 10;
    if (!is_ok(store.query_history(filtered_query, &result, &error))) {
        std::cerr << "filtered query failed: " << error << '\n';
        return 1;
    }
    if (!expect(result.total == 2 && result.rows.size() == 2, "filters must run over the database before pagination")) {
        return 1;
    }

    EventHistoryQuery escaped_search_query;
    escaped_search_query.search = "%";
    if (!is_ok(store.query_history(escaped_search_query, &result, &error))) {
        std::cerr << "escaped search query failed: " << error << '\n';
        return 1;
    }
    if (!expect(result.total == 1 && result.rows[0].target_id == "dev-a", "LIKE wildcards in user search must be treated literally")) {
        return 1;
    }

    EventHistoryQuery info_query;
    info_query.level = "info";
    info_query.page = 99;
    info_query.page_size = 1;
    if (!is_ok(store.query_history(info_query, &result, &error))) {
        std::cerr << "out-of-range page query failed: " << error << '\n';
        return 1;
    }
    if (!expect(result.total == 2 && result.rows.size() == 1 && result.rows[0].target_id == "config", "out-of-range pages must clamp to the last page")) {
        return 1;
    }

    EventHistoryQuery invalid_query;
    invalid_query.time_range = "1h";
    if (!expect(
            store.query_history(invalid_query, &result, &error) == StatusCode::kInvalidArgument,
            "unsupported time ranges must be rejected")) {
        return 1;
    }

    // 回归检查：历史页不得再被 list_recent_events 的 100 条上限截断。
    EventStore large_store;
    error.clear();
    if (!is_ok(large_store.initialize(":memory:", &error))) {
        std::cerr << "large store initialize failed: " << error << '\n';
        return 1;
    }
    for (std::uint32_t index = 0; index < 125; ++index) {
        auto event = make_event(
            now - index,
            "info",
            "system",
            "event-" + std::to_string(index),
            "history row " + std::to_string(index));
        if (!is_ok(large_store.append(std::move(event), &error))) {
            std::cerr << "large store append failed: " << error << '\n';
            return 1;
        }
    }
    EventHistoryQuery beyond_recent_limit_query;
    beyond_recent_limit_query.page = 11;
    beyond_recent_limit_query.page_size = 10;
    if (!is_ok(large_store.query_history(beyond_recent_limit_query, &result, &error))) {
        std::cerr << "beyond recent limit query failed: " << error << '\n';
        return 1;
    }
    if (!expect(
            result.total == 125 && result.rows.size() == 10 && result.rows.front().target_id == "event-100",
            "history pagination must reach rows beyond the most recent 100")) {
        return 1;
    }

    return 0;
}
