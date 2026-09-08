#pragma once
#include "data/model/service_summary.h"
#include <nlohmann/json.hpp>

namespace edge_controller {
inline nlohmann::json encode_service_event(const ServiceEvent& e) {
    const auto& d = e.diagnosis;
    return {{"event_id",e.event_id},{"first_timestamp_ms",e.first_timestamp_ms},{"timestamp_ms",e.timestamp_ms},
        {"level",e.level},{"source",e.source},{"target_id",e.target_id},{"summary",e.summary},{"detail",e.detail},
        {"occurrence_count",e.occurrence_count}, {"diagnosis", {
        {"level",d.level},{"target_id",d.target_id},{"target_name",d.target_name},{"status",d.status},
        {"error_code",d.error_code},{"message",d.message},{"suggestion",d.suggestion},
        {"last_success_time_ms",d.last_success_time_ms},{"last_error_time_ms",d.last_error_time_ms},
        {"consecutive_failures",d.consecutive_failures}}}};
}
inline ServiceEvent decode_service_event(const std::string& payload) {
    const auto j = nlohmann::json::parse(payload);
    ServiceEvent e;
    j.at("event_id").get_to(e.event_id); j.at("first_timestamp_ms").get_to(e.first_timestamp_ms);
    j.at("timestamp_ms").get_to(e.timestamp_ms); j.at("level").get_to(e.level);
    j.at("source").get_to(e.source); j.at("target_id").get_to(e.target_id);
    j.at("summary").get_to(e.summary); j.at("detail").get_to(e.detail);
    j.at("occurrence_count").get_to(e.occurrence_count);
    const auto& d = j.at("diagnosis");
    d.at("level").get_to(e.diagnosis.level); d.at("target_id").get_to(e.diagnosis.target_id);
    d.at("target_name").get_to(e.diagnosis.target_name); d.at("status").get_to(e.diagnosis.status);
    d.at("error_code").get_to(e.diagnosis.error_code); d.at("message").get_to(e.diagnosis.message);
    d.at("suggestion").get_to(e.diagnosis.suggestion);
    d.at("last_success_time_ms").get_to(e.diagnosis.last_success_time_ms);
    d.at("last_error_time_ms").get_to(e.diagnosis.last_error_time_ms);
    d.at("consecutive_failures").get_to(e.diagnosis.consecutive_failures);
    return e;
}
}  // namespace edge_controller
