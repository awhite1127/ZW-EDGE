// 时区、NTP 配置及系统/RTC 校时结果模型。
#pragma once

#include <cstdint>
#include <string>

#include "common/types.h"

namespace edge_controller {

inline constexpr const char kDefaultTimezone[] = "Asia/Shanghai";
inline constexpr const char kDefaultNtpPrimary[] = "ntp.aliyun.com";
inline constexpr const char kDefaultNtpSecondary[] = "pool.ntp.org";

struct TimeSettings {
    std::string timezone{kDefaultTimezone};
    bool ntp_enabled{false};
    std::string ntp_primary{kDefaultNtpPrimary};
    std::string ntp_secondary{kDefaultNtpSecondary};
    TimestampMs updated_at{0};
};

struct TimeSettingsUpdateRequest {
    std::string timezone;
    bool ntp_enabled{false};
    std::string ntp_primary;
    std::string ntp_secondary;
};

struct TimeApplyResult {
    TimeSettings settings;
    std::string message;
    std::string warning_message;
};

struct TimeSyncResult {
    bool synchronized{false};
    TimestampMs sync_time_ms{0};
    bool rtc_written{false};
    std::string message;
    std::string warning_message;
};

struct ManualTimeSetRequest {
    TimestampMs epoch_ms{0};
    std::string source{"manual"};
};

struct ManualTimeSetResult {
    bool time_set{false};
    TimestampMs previous_time_ms{0};
    TimestampMs current_time_ms{0};
    bool rtc_written{false};
    std::string message;
    std::string warning_message;
};

struct TimeRuntimeStatus {
    TimestampMs current_time_ms{0};
    std::string current_time_text;
    std::string timezone;
    std::string utc_offset_text;
    bool ntp_enabled{false};
    bool ntp_process_running{false};
    std::string ntp_process_state{"stopped"};
    std::string ntp_process_state_text{"NTP 未运行"};
    std::string sync_state{"disabled"};
    std::string sync_state_text{"NTP 已停用"};
    bool rtc_available{false};
    std::string rtc_time_text;
    TimestampMs last_sync_time_ms{0};
    std::string last_error_message;
    bool settings_pending_apply{false};
};

}  // namespace edge_controller
