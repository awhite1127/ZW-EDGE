// 封装 Linux 时区、NTP、RTC 与系统时间命令。
// 边界：系统命令和平台差异集中在本层，调用结果必须可诊断。

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "model/time_settings.h"

namespace edge_controller {

// LinuxTimeRuntime 是系统时间操作的唯一平台边界。上层不直接访问时区文件、
// ntpd、RTC、clock_settime、adjtimex 或 /proc。
class LinuxTimeRuntime {
public:
    struct Paths {
        std::string zoneinfo_directory{"/usr/share/zoneinfo"};
        std::string localtime_path{"/etc/localtime"};
        std::string timezone_path{"/etc/timezone"};
        std::string runtime_directory{"/opt/edge-controller/run"};
        std::string ntpd_path{"/usr/sbin/ntpd"};
        std::string hwclock_path{"/sbin/hwclock"};
        std::vector<std::string> rtc_paths{"/dev/rtc", "/dev/rtc0", "/dev/rtc1"};
        std::chrono::seconds rtc_probe_interval{std::chrono::seconds(60)};
        std::chrono::seconds rtc_failure_retry_interval{std::chrono::seconds(8)};
    };

    // 仅供平台实现完成失败回滚，不暴露给业务层使用。
    struct FileSnapshot {
        bool existed{false};
        bool was_symlink{false};
        std::string symlink_target;
        std::string content;
    };

    // 构造 LinuxTimeRuntime 实例。
    LinuxTimeRuntime();
    // 构造 LinuxTimeRuntime 实例。
    explicit LinuxTimeRuntime(Paths paths);

    // 校验服务端。
    static StatusCode validate_server(const std::string& server, std::string* error_message = nullptr);
    // 校验输入参数及其业务约束。
    static StatusCode validate_settings_shape(const TimeSettings& settings, std::string* error_message = nullptr);
    // 判断进程命令行是否属于本服务管理的 NTP 实例。
    static bool is_managed_ntpd_command_line(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        const std::string& expected_pid_file,
        const std::string& expected_config_file);
    // 校验手动设置的毫秒时间戳。
    static StatusCode validate_manual_epoch_ms(TimestampMs epoch_ms, std::string* error_message = nullptr);

    // 探测当前实际应用时区，用于运行态显示和失败后的状态快照。
    std::string detect_initial_timezone() const;
    // 根据系统运行态生成已应用时间设置快照。
    TimeSettings snapshot_applied_settings(const TimeSettings& known_settings) const;
    // 校验设置。
    StatusCode validate_settings(const TimeSettings& settings, std::string* error_message = nullptr) const;
    // 应用设置。
    StatusCode apply_settings(
        const TimeSettings& settings,
        const TimeSettings& previous_settings,
        std::string* warning_message = nullptr,
        std::string* error_message = nullptr);
    // 启动时恢复并应用持久化时间设置。
    StatusCode restore_on_startup(const TimeSettings& settings, std::string* error_message = nullptr);
    // 获取当前运行状态。
    TimeRuntimeStatus runtime_status(const TimeSettings& settings) const;
    // 立即执行一次网络时间同步。
    StatusCode sync_now(
        const TimeSettings& settings,
        TimeSyncResult* result,
        std::chrono::seconds timeout = std::chrono::seconds(45),
        std::string* error_message = nullptr);
    // 设置系统与硬件时钟并返回校时结果。
    StatusCode set_manual_time(
        const TimeSettings& settings,
        TimestampMs epoch_ms,
        ManualTimeSetResult* result,
        std::string* error_message = nullptr);
    // 关闭。
    void shutdown();

private:
    // 校验时区名称和对应系统文件。
    StatusCode validate_timezone(const std::string& timezone, std::string* error_message) const;
    // 应用系统时区设置。
    StatusCode apply_timezone(const std::string& timezone, std::string* error_message);
    // 根据配置协调受管与外部 NTP 进程。
    StatusCode reconcile_ntpd(const TimeSettings& settings, std::string* error_message);
    // 启动本服务管理的 NTP 进程。
    StatusCode start_managed_ntpd(const TimeSettings& settings, std::string* error_message);
    // 停止本服务管理的 NTP 进程。
    StatusCode stop_managed_ntpd(std::string* error_message);
    // 停止外部 NTP 进程并接管时间同步。
    StatusCode take_over_external_ntpd(std::string* error_message);
    // 停止不受本服务管理的外部 NTP 进程。
    StatusCode stop_external_ntpd(const std::vector<int>& pids, std::string* error_message) const;
    // 判断受管 NTP 进程是否正在运行。
    bool managed_ntpd_running(int* pid = nullptr) const;
    // 枚举不受本服务管理的外部 NTP 进程。
    std::vector<int> external_ntpd_pids() const;
    // 判断是否存在外部 NTP 进程。
    bool external_ntpd_running() const;
    // 状态页短时复用外部进程探测；会改变进程的操作始终调用上面的实时版本。
    bool external_ntpd_running_for_status() const;
    // 写入受管 NTP 进程配置。
    StatusCode write_ntp_config(const TimeSettings& settings, std::string* error_message) const;
    // 将系统时间写入硬件时钟。
    StatusCode write_rtc(std::string* warning_message) const;
    // 读取硬件时钟时间。
    std::string read_rtc_time(bool* available) const;
    // 使硬件时钟运行状态缓存失效。
    void invalidate_rtc_cache() const;
    // 解析并返回对应文件路径。
    std::string pid_file_path() const;
    // 解析并返回对应文件路径。
    std::string ntp_config_path() const;

    Paths paths_;
    // 串行化会改变系统状态的操作；状态锁只保护可快速读取的内存字段。
    mutable std::mutex operation_mutex_;
    mutable std::mutex mutex_;
    mutable std::mutex rtc_cache_mutex_;
    mutable std::mutex external_ntpd_cache_mutex_;
    mutable bool external_ntpd_cache_initialized_{false};
    mutable bool external_ntpd_cache_running_{false};
    mutable std::chrono::steady_clock::time_point external_ntpd_cache_checked_at_{};
    mutable bool rtc_cache_initialized_{false};
    mutable bool rtc_cache_available_{false};
    mutable std::string rtc_cache_time_text_;
    mutable std::chrono::steady_clock::time_point rtc_cache_checked_at_{};
    bool synchronizing_{false};
    bool ntpd_takeover_in_progress_{false};
    bool ntpd_takeover_failed_{false};
    TimestampMs last_sync_time_ms_{0};
    std::string last_error_message_;
};

}  // namespace edge_controller
